// -----------------------------------------------------------------------------
// vscode-vamiga-debugger dma profiler
//
// A per-DMA-cycle bus profiler, sibling of CpuProfiler. While enabled it records,
// for every bus cycle of one frame, a compact { owner, flags, data, addr } entry
// (the "enriched grid"), plus an initial chip/slow-RAM snapshot. The grid drives
// two things in the host (VS Code extension):
//   * visualization — a DMA "channel line" in the flame graph + per-channel totals
//     in the time view (colored by bus owner; CPU split Code/Data; Copper MOVE/
//     WAIT/SKIP), and
//   * memory reconstruction (future) — replaying the grid's WRITE cells over the
//     snapshot reconstructs chip/slow RAM (and custom registers) at any cycle. The
//     slot's position in the grid IS its timestamp; no per-event clock is stored.
//
// Why a grid instead of WinUAE's per-cycle dma_rec copy: vAmiga already records
// busOwner/busAddr/busData per line (Agnus), which we fold into a frame-wide buffer
// at EOL. The two things the bus arrays DON'T carry — read-vs-write and byte-vs-word
// — we add as a per-cycle `flags` byte stamped at the write sites; the CPU Code/Data
// bit comes from Moira's function code, the Copper sub-state from the copper command.
//
// This whole module is fork-local — upstream vAmiga has no file here, so it never
// causes merge conflicts. It is wired in via a few one-line hooks tagged
//   // [vscode-vamiga-debugger dma profiler]
// in Agnus.cpp / AgnusEvents.cpp / AgnusDma.cpp / Memory.cpp / main.cpp. See
// FORK_NOTES.md.
// -----------------------------------------------------------------------------

#pragma once

#include "BasicTypes.h"

namespace vamiga {

class Memory;

namespace DmaProfiler {

// Fixed PAL grid geometry, matching the old vscode-amiga-debug NR_DMA_REC_HPOS/VPOS. The
// grid is always exactly DMA_HPOS*DMA_VPOS cells so the host can decode a flat slot index
// into (line, colour-clock) with the same constants — no runtime dimension is shipped.
// PAL only (vAmiga is OCS/ECS); the emulator's busOwner[] arrays are sized for the NTSC
// long line (228), but PAL only uses cycles 0..226, so we copy DMA_HPOS per line.
static constexpr u32 DMA_HPOS = 227;
static constexpr u32 DMA_VPOS = 313;

// One grid cell per DMA cycle. Naturally 8-byte aligned (no padding); the
// static_assert locks the layout the TS decoder relies on. Stored native-endian
// (wasm is little-endian, matching the host's typed-array reads).
struct Cell {
    u8  owner;   // BusOwner ordinal (0 = NONE)
    u8  flags;   // see DMA_* below
    u16 data;    // bus data
    u32 addr;    // bus address
};
static_assert(sizeof(Cell) == 8, "DmaProfiler::Cell must be exactly 8 bytes");

// `flags` bits. Low 3 are general; bits 3-4 are an owner-specific sub-state
// (currently only Copper: 0 = MOVE/fetch, 1 = WAIT, 2 = SKIP).
static constexpr u8 DMA_WRITE     = 1 << 0; // this cycle wrote memory / a register
static constexpr u8 DMA_BYTE      = 1 << 1; // byte access (else word)
static constexpr u8 DMA_CODE      = 1 << 2; // CPU instruction fetch (PROG space)
static constexpr u8 DMA_SUB_SHIFT = 3;
static constexpr u8 DMA_SUB_MASK  = u8(0x3 << DMA_SUB_SHIFT);

// Copper sub-states packed into DMA_SUB_MASK.
static constexpr u8 COP_SUB_MOVE = 0;
static constexpr u8 COP_SUB_WAIT = 1;
static constexpr u8 COP_SUB_SKIP = 2;

// Capture-enabled flag. An inline variable (not hidden in the .cpp) so the hot-path
// hooks below can gate on `enabled()` WITHOUT a cross-TU function call when capture is
// off — normal emulation pays only an inlined load+branch. Set by start()/stop().
inline bool gCaptureEnabled = false;
inline bool enabled() { return gCaptureEnabled; }

// Wiring (called from main.cpp): the memory the snapshot is copied from.
void setMemory(Memory *mem);

// Capture control. start() snapshots chip/slow RAM, clears the grid, and enables
// the per-cycle hooks; stop() disables them. Bracket one frame, frame-aligned,
// exactly like CpuProfiler.
void start();
void stop();

// Per-line copy at end-of-line: fold Agnus's busOwner/busAddr/busData for the whole
// line (plus the sub-state flags accumulated by the mark* hooks) into the frame grid
// at row `vpos`, then clear the per-line flag scratch. `owner` points at
// BusOwner[HPOS_CNT] (1 byte each); `addr`/`data` are Agnus's u32/u16 line arrays.
void recordLine(isize vpos, const void *owner, const u32 *addr, const u16 *data);

// Per-cycle marks (no-ops unless enabled). They set bits in the current line's flag
// scratch at column `hpos`, picked up by the next recordLine.
void markWrite(isize hpos, bool isByte); // DMA_WRITE [| DMA_BYTE]
void markCpu(isize hpos, bool isCode);   // DMA_CODE on CPU instruction fetches
void markCopper(isize hpos, u8 subState);// Copper MOVE/WAIT/SKIP sub-state

// Readback for the wasm layer (valid until the next start()). Pointers are into
// internal buffers; lengths are in bytes.
const u8 *gridData();   u32 gridLen();   // Cell[DMA_HPOS*DMA_VPOS] — the enriched grid
const u8 *chipData();   u32 chipLen();   // chip RAM snapshot at capture start
const u8 *slowData();   u32 slowLen();   // slow/bogo RAM snapshot (may be empty)
const u8 *customData(); u32 customLen(); // custom-register baseline (256 u16, little-endian)

}
}
