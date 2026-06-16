// -----------------------------------------------------------------------------
// vscode-vamiga-debugger dma profiler
// See DmaProfiler.h for the overview.
// -----------------------------------------------------------------------------

#include "DmaProfiler.h"
#include "Memory.h"      // Memory members (chip/slow pointers + sizes)
#include <vector>
#include <cstring>

namespace vamiga {
namespace DmaProfiler {

namespace {

Memory *gMem = nullptr;

// Fixed-geometry grid: DMA_VPOS rows × DMA_HPOS cells. Sized once at start().
std::vector<Cell> gGrid;
constexpr isize gStride = DMA_HPOS; // cells per line (PAL)
constexpr isize gRows = DMA_VPOS;   // lines (PAL)

// Per-line flag scratch, indexed by hpos. Accumulated by the mark* hooks during a
// line, folded into the grid (and cleared) by recordLine at EOL.
std::vector<u8> gLineFlags;

// Capture-start RAM snapshots (reconstruction baseline).
std::vector<u8> gChip;
std::vector<u8> gSlow;

// Capture-start custom-register baseline: 256 u16 (0xDFF000..0x1FE), little-endian.
std::vector<u8> gCustom;

} // anonymous namespace

void setMemory(Memory *mem) { gMem = mem; }

void start()
{
    gGrid.assign(size_t(gStride * gRows), Cell{});
    gLineFlags.assign(size_t(gStride), 0);

    // Snapshot chip + slow RAM as the reconstruction baseline.
    if (gMem && gMem->chip) {
        gChip.assign(gMem->chip, gMem->chip + gMem->chipRamSize());
    } else {
        gChip.clear();
    }
    if (gMem && gMem->slow && gMem->slowRamSize() > 0) {
        gSlow.assign(gMem->slow, gMem->slow + gMem->slowRamSize());
    } else {
        gSlow.clear();
    }

    // Custom-register baseline: a side-effect-free spypeek of the whole 0xDFF000..0x1FE
    // range ("we get what we can" — the old vscode-amiga-debug shipped a full register
    // file from WinUAE's save_custom; vAmiga has no flat dump). spypeekCustom16 returns
    // the readable registers accurately and 0 for write-only ones, so as a final step we
    // backfill the write-only DMACON (0x096) from its readable mirror DMACONR (0x002),
    // which carries the channel-enable + BLTPRI bits — that's all the DMA-Control view
    // needs, and per-slot state is recovered by replaying the frame's WRITE cells over
    // this baseline (reconstructCustomRegs, host-side). Other write-only regs start at 0.
    gCustom.assign(256 * 2, 0);
    if (gMem) {
        for (u32 off = 0; off < 0x200; off += 2) {
            u16 v = gMem->spypeekCustom16(0xDFF000 + off);
            gCustom[off]     = u8(v & 0xff);
            gCustom[off + 1] = u8(v >> 8);
        }
        // DMACON (0x096) <- DMACONR (0x002)
        gCustom[0x096] = gCustom[0x002];
        gCustom[0x097] = gCustom[0x003];
    }

    gCaptureEnabled = true;
}

void stop() { gCaptureEnabled = false; }

// The mark*/recordLine bodies assume the caller already checked enabled() (the hooks
// gate inline via DmaProfiler::enabled() so normal emulation pays no call). They keep
// only the cheap bounds checks.

void recordLine(isize vpos, const void *owner, const u32 *addr, const u16 *data)
{
    if (vpos < 0 || vpos >= gRows) return;

    const u8 *ownerBytes = reinterpret_cast<const u8 *>(owner);
    Cell *row = gGrid.data() + vpos * gStride;

    for (isize h = 0; h < gStride; h++) {
        row[h].owner = ownerBytes[h];
        row[h].flags = gLineFlags[size_t(h)];
        row[h].data  = data[h];
        row[h].addr  = addr[h];
    }

    std::memset(gLineFlags.data(), 0, gLineFlags.size());
}

void markWrite(isize hpos, bool isByte)
{
    if (hpos < 0 || hpos >= gStride) return;
    gLineFlags[size_t(hpos)] |= u8(DMA_WRITE | (isByte ? DMA_BYTE : 0));
}

void markCpu(isize hpos, bool isCode)
{
    if (!isCode) return;
    if (hpos < 0 || hpos >= gStride) return;
    gLineFlags[size_t(hpos)] |= DMA_CODE;
}

void markCopper(isize hpos, u8 subState)
{
    if (hpos < 0 || hpos >= gStride) return;
    u8 &f = gLineFlags[size_t(hpos)];
    f = u8((f & ~DMA_SUB_MASK) | ((subState << DMA_SUB_SHIFT) & DMA_SUB_MASK));
}

const u8 *gridData() { return reinterpret_cast<const u8 *>(gGrid.data()); }
u32 gridLen()        { return u32(gGrid.size() * sizeof(Cell)); }

const u8 *chipData() { return gChip.data(); }
u32 chipLen()        { return u32(gChip.size()); }

const u8 *slowData() { return gSlow.data(); }
u32 slowLen()        { return u32(gSlow.size()); }

const u8 *customData() { return gCustom.data(); }
u32 customLen()        { return u32(gCustom.size()); }

}
}
