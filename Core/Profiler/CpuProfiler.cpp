// -----------------------------------------------------------------------------
// vscode-vamiga-debugger cpu profiler
// See CpuProfiler.h for the overview.
// -----------------------------------------------------------------------------

#include "CpuProfiler.h"
#include "Memory.h"   // Memory::spypeek32<Accessor::CPU>
#include <vector>
#include <cstdio>

namespace vamiga {
namespace CpuProfiler {

namespace {

Memory *gMem = nullptr;
std::vector<u8> gUnwind;          // raw UnwindEntry[] bytes (indexed by (pc-gStart)/2)
u32 gStart = 0, gEnd = 0;         // loaded program text range
bool gEnabled = false;
std::vector<u32> gOutput;         // flat per-instruction records

// Pending pre-instruction snapshot (set by beginInstr, consumed by endInstr).
bool gPending = false;
u32 gPc = 0, gA5 = 0, gA7 = 0;
i64 gClock = 0;

// Diagnostics (reset on start): how many instructions were profiled this capture,
// and how many fell inside the program text range. Surfaced to help diagnose an
// empty capture (e.g. the frame ran only OS code).
u32 gTotalInstr = 0, gInRange = 0;

constexpr u32 kMaxDepth = 64;     // call-stack depth cap (runaway / recursion guard)

inline const UnwindEntry *entryFor(u32 pc)
{
    if (pc < gStart || pc >= gEnd) return nullptr;
    u32 idx = (pc - gStart) >> 1;
    if ((idx + 1) * sizeof(UnwindEntry) > gUnwind.size()) return nullptr;
    return reinterpret_cast<const UnwindEntry *>(gUnwind.data()) + idx;
}

} // anonymous namespace

void setMemory(Memory *mem) { gMem = mem; }

void setUnwind(const u8 *data, u32 len, u32 startAddr, u32 endAddr)
{
    gUnwind.assign(data, data + len);
    gStart = startAddr;
    gEnd = endAddr;
}

void start()
{
    gOutput.clear();
    gPending = false;
    gTotalInstr = 0;
    gInRange = 0;
    gEnabled = true;
}

void stop()
{
    gEnabled = false;
    gPending = false;
    printf("[cpu-profiler] stop: profiled %u instr, %u in range, output=%u words\n",
           gTotalInstr, gInRange, (u32)gOutput.size());
}

void beginInstr(u32 pc, u32 a5, u32 a7, i64 clock)
{
    gPc = pc; gA5 = a5; gA7 = a7; gClock = clock; gPending = true;
}

// Registers are captured pre-instruction (beginInstr) but memory is read post-instruction
// (here, via spypeek32). For standard m68k calling convention this is safe: stack-growing
// writes always go downward to [A7-n] (newly allocated space), while the DWARF unwind reads
// upward from [A7+n] (existing caller frames). These regions are disjoint for every normal
// prologue/epilogue instruction (move.l -(sp), jsr, rts, addq sp). The only case that would
// cause a mismatch is code that writes to a positive sp+offset (modifying a caller's frame),
// which well-behaved code never does. If that ever becomes an issue, move the spypeek32 calls
// into beginInstr alongside the register snapshot.
void endInstr(i64 clock)
{
    if (!gPending) return;
    gPending = false;
    if (!gEnabled || !gMem) return;

    gTotalInstr++;

    // Only profile instructions inside the program's text range.
    if (gPc < gStart || gPc >= gEnd) return;

    gInRange++;

    // Reconstruct the call stack leaf-first, exactly like WinUAE: the leaf is the
    // current PC; each caller's return address lives at CFA+ra, and the caller's
    // A5 (needed when an outer frame's CFA is A5-relative) at CFA+r13.
    u32 stack[kMaxDepth];
    u32 depth = 0;
    u32 pc = gPc, a5 = gA5, a7 = gA7;

    while (depth < kMaxDepth && pc >= gStart && pc < gEnd) {

        stack[depth++] = pc;

        const UnwindEntry *e = entryFor(pc);
        if (!e || e->cfa == 0) break;            // no unwind info -> stop

        u32 cfaReg = (e->cfa >> 12) & 0xf;
        u32 cfaOfs = e->cfa & 0xfff;
        u32 base = (cfaReg == 13) ? a5 : a7;     // A5-based frame vs SP/A7-based
        u32 cfa = base + cfaOfs;

        u32 ret = gMem->spypeek32<Accessor::CPU>(cfa + (u32)(i32)e->ra);
        if (e->r13 != 0) a5 = gMem->spypeek32<Accessor::CPU>(cfa + (u32)(i32)e->r13);
        if (cfa <= a7) break;                    // CFA must advance (stack grows down); same CFA = loop
        a7 = cfa;                                // caller's SP is the CFA
        pc = ret;
    }

    // Append: [depth, pc..., cycleDelta]
    gOutput.push_back(depth);
    for (u32 i = 0; i < depth; i++) gOutput.push_back(stack[i]);
    gOutput.push_back((u32)(clock - gClock));
}

const u32 *data() { return gOutput.data(); }
u32 count() { return (u32)gOutput.size(); }

// Diagnostics for an empty/short capture.
u32 totalInstr() { return gTotalInstr; }
u32 inRangeInstr() { return gInRange; }
u32 rangeStart() { return gStart; }
u32 rangeEnd() { return gEnd; }

}
}
