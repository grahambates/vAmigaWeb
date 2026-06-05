// -----------------------------------------------------------------------------
// vscode-vamiga-debugger cpu profiler
//
// A per-instruction CPU profiler ported from the vscode-amiga-debug / WinUAE
// "cpu_profiler". While enabled it records, for every executed instruction in the
// loaded program's text range, the reconstructed call stack (via DWARF CFA
// unwinding of A5/A7) and the elapsed cycle count. The host (VS Code extension)
// symbolicates and aggregates the flat output into a call tree / flame graph.
//
// This whole module is fork-local — upstream vAmiga has no file here, so it never
// causes merge conflicts. It is wired in via a few one-line hooks tagged
//   // [vscode-vamiga-debugger cpu profiler]
// in Moira.cpp / MoiraTypes.h / CPU.{h,cpp} / main.cpp. See FORK_NOTES.md.
// -----------------------------------------------------------------------------

#pragma once

#include "BasicTypes.h"

namespace vamiga {

class Memory;

namespace CpuProfiler {

// One unwind entry per 2-byte code location. Mirrors WinUAE's cpu_profiler_unwind
// and the TS-side packer in src/unwindTable.ts:
//   cfa = (cfaReg << 12) | cfaOffset, r13/ra = byte offsets from CFA.
// All members are 2-byte aligned, so the struct is naturally exactly 6 bytes with
// no padding on every compiler — no #pragma pack needed (the static_assert locks it).
struct UnwindEntry { u16 cfa; i16 r13; i16 ra; };
static_assert(sizeof(UnwindEntry) == 6, "UnwindEntry must match the 6-byte packed layout");

// Wiring (called from main.cpp): the memory the unwinder reads the stack from,
// and the per-location unwind table covering the program's [startAddr,endAddr).
void setMemory(Memory *mem);
void setUnwind(const u8 *data, u32 len, u32 startAddr, u32 endAddr);

// Capture control. start() clears the output buffer and enables capture; the
// Moira PROFILING flag gates whether the per-instruction hooks below fire.
void start();
void stop();

// Per-instruction hooks, called from Moira::execute() (slow path) only when the
// PROFILING flag is set. beginInstr stashes the pre-execution PC/A5/A7 + clock;
// endInstr computes the cycle delta, unwinds the call stack, and appends a record.
void beginInstr(u32 pc, u32 a5, u32 a7, i64 clock);
void endInstr(i64 clock);

// Raw output: a flat u32 stream of per-instruction records, leaf-first:
//   [depth, pc0, pc1, ... pc(depth-1), cycleDelta]
// PCs are absolute loaded addresses; depth >= 1. Used by the wasm export.
const u32 *data();
u32 count(); // number of u32 words in data()

// Diagnostics from the last capture (for tracing an empty/short result).
u32 totalInstr();   // instructions profiled (in the captured frame)
u32 inRangeInstr(); // ...of those, how many fell inside [start,end)
u32 rangeStart();
u32 rangeEnd();

}
}
