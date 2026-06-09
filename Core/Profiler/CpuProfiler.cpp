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
bool gSampleSuper = false;        // S-bit of the pending sample (which branch-stack it belongs to)
i64 gClock = 0;

// Diagnostics (reset on start): how many instructions were profiled this capture,
// and how many fell inside the program text range. Surfaced to help diagnose an
// empty capture (e.g. the frame ran only OS code).
u32 gTotalInstr = 0, gInRange = 0;

// [IRQ] cycle-gap tracking (WinUAE cpu_profiler mechanism): the clock at the end of the
// previous profiled instruction. When an interrupt/exception is dispatched, the CPU runs
// via checkForIrq/execException -> goto done, skipping beginInstr; that path lands in
// endInstr with no pending instruction but with the clock advanced by the dispatch
// overhead. endInstr emits that advance as a standalone [IRQ] marker (the gap is reliably
// observable there, not at the following beginInstr). Seeded in start() with the capture-
// start clock so a dispatch before the first instruction is still attributed.
i64 gLastEndClock = 0;
bool gHaveLastEnd = false;

constexpr u32 kMaxDepth = 64;     // call-stack depth cap (runaway / recursion guard)

// Branch-stack mode only: the shadow call stack is reconstructed PRE-instruction (in
// beginInstr) and stashed here, then emitted in endInstr. This mirrors the DWARF
// path's pre-instruction register discipline and is essential, because the shadow
// stack is mutated by the JSR/BSR/RTS/RTE hooks DURING execute(): snapshotting it
// after (in endInstr) would attribute the call/return instruction itself to the
// post-push/pop stack (a spurious extra frame on JSR, a missing caller on RTS).
// (DWARF reads caller frames from memory, which the instruction does not touch, so it
// can stay in endInstr — see the note there.)
u32 gBranchStack[kMaxDepth];
u32 gBranchDepth = 0;

// --- Runtime branch-stack (no-DWARF fallback) --------------------------------
// Ported from WinUAE's debugmem.cpp (branch_stack_push / _pop_rts / _pop_rte): a
// shadow call stack built from JSR/BSR/RTS/RTE + exception-entry hooks. WinUAE's
// debugstackframe, trimmed to the fields we emit (no regs[16]/sr snapshot):
//   returnPC <- WinUAE next_pc (the value the matching RTS/RTE returns to = pop key)
//   a7AtCall <- WinUAE stack   (A7 at push; diagnostics only, not the pop key)
struct BranchFrame { u32 returnPC; u32 a7AtCall; };
constexpr u32 kMaxFrames = 256;   // WinUAE used 100; raised (overflow drops oldest, see push)

// Two stacks, exactly like WinUAE's stackframes / stackframessuper, selected by the
// 68000 S-bit. On the m68k A7 aliases USP (S=0) or SSP (S=1); exceptions/IRQs switch
// to SSP, so user- and supervisor-mode return addresses must not share one stack.
BranchFrame gUserFrames[kMaxFrames];  u32 gUserCount  = 0;
BranchFrame gSuperFrames[kMaxFrames]; u32 gSuperCount = 0;

// Reconstruction method, chosen in start(): branch-stack when no unwind table was
// uploaded (assembly), else DWARF.
enum class Method { Dwarf, Branch };
Method gMethod = Method::Dwarf;

// Branch-stack seeding (see seedBranchStacks): the shadow stacks start empty mid-frame,
// so seed them once from the live stack(s) on the first profiled instruction.
bool gSeeded = false;

inline const UnwindEntry *entryFor(u32 pc)
{
    if (pc < gStart || pc >= gEnd) return nullptr;
    u32 idx = (pc - gStart) >> 1;
    if ((idx + 1) * sizeof(UnwindEntry) > gUnwind.size()) return nullptr;
    return reinterpret_cast<const UnwindEntry *>(gUnwind.data()) + idx;
}

// Append a frame, dropping the oldest on overflow. This is the one intentional
// deviation from WinUAE, which RESETS the whole stack to 0 on overflow (losing all
// frames — the behaviour that loses deep stacks); dropping the oldest preserves the
// recent, relevant frames. Both are crash-safe (the unbounded write that forced the
// WinUAE fork to disable this is avoided either way).
inline void pushFrame(BranchFrame *frames, u32 &count, u32 returnPC, u32 a7AtCall)
{
    if (count >= kMaxFrames) {
        for (u32 i = 1; i < kMaxFrames; i++) frames[i - 1] = frames[i];
        count = kMaxFrames - 1;
    }
    frames[count++] = { returnPC, a7AtCall };
}

// DWARF call-stack reconstruction (primary): leaf-first, exactly like WinUAE — the
// leaf is the current PC; each caller's return address lives at CFA+ra, and the
// caller's A5 (needed when an outer frame's CFA is A5-relative) at CFA+r13.
u32 unwindDwarf(u32 *stack, u32 cap)
{
    u32 depth = 0;
    u32 pc = gPc, a5 = gA5, a7 = gA7;

    while (depth < cap && pc >= gStart && pc < gEnd) {

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
    return depth;
}

// Branch-stack call-stack reconstruction (fallback): leaf-first snapshot of the
// shadow stack. The active stack is selected by the sampled S-bit; when sampling in
// supervisor mode the user stack is appended beneath (the enterException frame
// bridges handler -> interrupted user code -> its callers), so the flame graph sees
// the full root->leaf path even across an IRQ.
u32 unwindBranch(u32 *stack, u32 cap)
{
    u32 depth = 0;
    stack[depth++] = gPc;                                  // leaf = current PC (raw)

    const BranchFrame *active = gSampleSuper ? gSuperFrames : gUserFrames;
    u32 activeCount          = gSampleSuper ? gSuperCount  : gUserCount;
    for (i32 i = (i32)activeCount - 1; i >= 0 && depth < cap; i--)
        stack[depth++] = active[i].returnPC;

    if (gSampleSuper)
        for (i32 i = (i32)gUserCount - 1; i >= 0 && depth < cap; i--)
            stack[depth++] = gUserFrames[i].returnPC;

    return depth;
}

// Seed a branch stack from the live machine stack at capture start, so samples before
// the first observed JSR/BSR still carry their existing caller chain. Mirrors the
// host-side heuristic in src/stackManager.ts guessStack (KEEP THE TWO IN SYNC): scan
// 128 bytes from SP word-by-word; accept a longword that is an even, in-range code
// address whose preceding 3 words contain a JSR (w & 0xffc0)==0x4e80 or
// BSR (w & 0xff00)==0x6100; advance 4 on a hit, 2 otherwise. Best-effort: false
// positives (data shaped like a return addr after a call-shaped word) and false
// negatives (PEA+RTS, JMP tables) are possible — those frames just appear once a real
// JSR/BSR is observed. (WinUAE didn't seed at all and built up from empty.)
void seedStack(BranchFrame *frames, u32 &count, u32 sp)
{
    if (!gMem) return;

    constexpr u32 kSeedBytes = 128;
    BranchFrame cand[kMaxFrames];
    u32 c = 0;
    u32 off = 0;
    while (off + 4 <= kSeedBytes && c < kMaxFrames) {
        u32 slot = sp + off;
        u32 addr = gMem->spypeek32<Accessor::CPU>(slot);
        if (addr > 0x100 && !(addr & 1) && addr >= gStart && addr < gEnd) {
            bool isCall = false;
            for (int i = 0; i < 3; i++) {
                u16 w = gMem->spypeek16<Accessor::CPU>(addr - 6 + (u32)(i * 2));
                if ((w & 0xffc0) == 0x4e80 || (w & 0xff00) == 0x6100) { isCall = true; break; }
            }
            if (isCall) { cand[c++] = { addr, slot }; off += 4; continue; }
        }
        off += 2;
    }
    // Stack memory near SP holds the most-recent (innermost) returns, so we discovered
    // innermost-first; push outermost-first to match real push order.
    for (i32 i = (i32)c - 1; i >= 0; i--)
        pushFrame(frames, count, cand[i].returnPC, cand[i].a7AtCall);
}

// Seed both shadow stacks at capture start. Capture can begin in EITHER mode — and on
// the Amiga it very often begins mid-interrupt (the frame-aligned start lands right
// after VERTB fires), so the active A7 may be the SSP. The USER stack must therefore be
// seeded from the real user SP: when in supervisor mode that is the saved `usp`, not the
// active A7 (scanning the SSP there would lose the user call chain — the program's
// _start/main frames — until the next user JSR/BSR rebuilds them). When we start in
// supervisor mode there may also be a live handler call chain on the SSP, so seed the
// supervisor stack from the active A7 too. One unavoidable gap: the exception-entry frame
// of an interrupt already in flight at capture start is not recoverable by the JSR/BSR
// heuristic (its return PC was stacked by the CPU, not pushed by a call), so that one
// in-flight handler shows the user chain directly as its caller, without the precise
// interrupted-PC bridge; handlers entered after capture starts get the bridge via
// enterException.
void seedBranchStacks(u32 a7, u32 usp, bool super)
{
    seedStack(gUserFrames, gUserCount, super ? usp : a7);
    if (super) seedStack(gSuperFrames, gSuperCount, a7);
}

} // anonymous namespace

void setMemory(Memory *mem) { gMem = mem; }

void setUnwind(const u8 *data, u32 len, u32 startAddr, u32 endAddr)
{
    gUnwind.assign(data, data + len);
    gStart = startAddr;
    gEnd = endAddr;
}

void start(i64 startClock)
{
    gOutput.clear();
    gPending = false;
    gTotalInstr = 0;
    gInRange = 0;
    // Reset the branch-stack and pick the method: branch-stack when no DWARF unwind
    // table was uploaded (assembly / hunk), else DWARF.
    gUserCount = 0;
    gSuperCount = 0;
    gSeeded = false;
    // Seed the [IRQ] gap tracker with the capture-start (frame-boundary) clock. Capture is
    // frame-aligned, so a VERTB interrupt is typically dispatched BEFORE the first profiled
    // instruction; seeding (rather than suppressing the first gap) attributes that dispatch
    // to [IRQ] instead of dropping its cycles. A capture that starts cleanly on an
    // instruction has a zero first gap, so no spurious [IRQ] is emitted.
    gLastEndClock = startClock;
    gHaveLastEnd = true;
    gMethod = gUnwind.empty() ? Method::Branch : Method::Dwarf;
    gEnabled = true;
}

void stop()
{
    gEnabled = false;
    gPending = false;
    printf("[cpu-profiler] stop: method=%s, profiled %u instr, %u in range, output=%u words\n",
           gMethod == Method::Branch ? "branch-stack" : "dwarf",
           gTotalInstr, gInRange, (u32)gOutput.size());
}

void beginInstr(u32 pc, u32 a5, u32 a7, u32 usp, bool super, i64 clock)
{
    gPc = pc; gA5 = a5; gA7 = a7; gSampleSuper = super; gClock = clock; gPending = true;

    // Branch-stack mode: seed the shadow stack(s) from the live stack on the first
    // profiled instruction (capture starts mid-frame — often mid-interrupt — so the
    // stacks are non-empty but unobserved). Lazy here rather than in start() so the live
    // SP and S-bit are used. `usp` is the saved user SP (valid when super); seeding needs
    // the real user SP, which is the active a7 in user mode but `usp` in supervisor mode.
    if (gMethod == Method::Branch) {
        if (!gSeeded) {
            gSeeded = true;
            seedBranchStacks(a7, usp, super);
        }
        // Snapshot the call stack NOW (pre-instruction), before this instruction's
        // JSR/BSR/RTS/RTE hook mutates the shadow stack. endInstr emits this stash.
        gBranchDepth = unwindBranch(gBranchStack, kMaxDepth);
    }
}

// --- Branch-stack hooks (called from Moira; no-ops unless branch-stack is active) --
namespace BranchStack {

// WinUAE branch_stack_push: record a return address on the S-selected stack. A
// user-mode push also clears the supervisor stack (WinUAE: stackframecntsuper = 0),
// discarding leftover super frames from an unbalanced RTE.
void push(bool super, u32 returnPC, u32 a7AtCall)
{
    if (gMethod != Method::Branch) return;
    if (super) {
        pushFrame(gSuperFrames, gSuperCount, returnPC, a7AtCall);
    } else {
        pushFrame(gUserFrames, gUserCount, returnPC, a7AtCall);
        gSuperCount = 0;
    }
}

// WinUAE branch_stack_pop_rts: scan the S-selected stack top-down for the frame whose
// returnPC matches; pop it and everything above (survives longjmp/multi-frame unwind;
// nearest match wins for recursion). No match -> no-op. A user-mode pop also clears
// the supervisor stack (WinUAE cleanup).
void popRts(bool super, u32 returnPC)
{
    if (gMethod != Method::Branch) return;
    if (super) {
        for (i32 i = (i32)gSuperCount - 1; i >= 0; i--)
            if (gSuperFrames[i].returnPC == returnPC) { gSuperCount = (u32)i; break; }
    } else {
        for (i32 i = (i32)gUserCount - 1; i >= 0; i--)
            if (gUserFrames[i].returnPC == returnPC) { gUserCount = (u32)i; break; }
        gSuperCount = 0;
    }
}

// WinUAE branch_stack_pop_rte: always unwinds the SUPERVISOR stack (the exception
// frame was pushed there; SR/mode is already restored by the time this runs). On a
// match, pop to it; on no match, leave the stack intact (see below).
void popRte(u32 returnPC)
{
    if (gMethod != Method::Branch) return;
    for (i32 i = (i32)gSuperCount - 1; i >= 0; i--)
        if (gSuperFrames[i].returnPC == returnPC) { gSuperCount = (u32)i; return; }
    // No match: no-op. (WinUAE pops one frame here — "assume it matched" — but for a
    // one-frame profile capture a no-match RTE almost always means an interrupt that was
    // ALREADY IN FLIGHT when the capture started: its entry never ran enterException
    // (it fired during the pre-capture alignment frame, before PROFILING), so the
    // returning PC matches nothing. Popping a frame would then discard a *legitimate*
    // caller (the interrupted function's own return frame) and make that function look
    // like a root until the next call rebuilds it. Leaving the stack intact is correct
    // here; genuinely unbalanced super frames are cleared by the user-push reset.)
}

// WinUAE pushes exception/IRQ entry via the same branch_stack_push in supervisor
// mode; this is push() onto the supervisor stack with the interrupted PC.
void enterException(u32 returnPC, u32 a7)
{
    if (gMethod != Method::Branch) return;
    pushFrame(gSuperFrames, gSuperCount, returnPC, a7);
}

} // namespace BranchStack

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
    if (!gPending) {
        // A goto-done with no pending instruction = the interrupt/exception dispatch path
        // (checkForIrq / execException jump straight to done, skipping beginInstr). The clock
        // has advanced by the dispatch overhead since the previous instruction ended; emit
        // that as an [IRQ] marker HERE — it's reliably observable at the dispatch site, unlike
        // the following beginInstr (vAmiga doesn't surface it there). This is WinUAE's
        // cycle-gap [IRQ], relocated to where vAmiga makes it visible. Advance gLastEndClock so
        // the handler's first instruction doesn't re-count it.
        if (gEnabled && gHaveLastEnd && clock > gLastEndClock) {
            gOutput.push_back(1);
            gOutput.push_back(IRQ_MARKER);
            gOutput.push_back((u32)(clock - gLastEndClock));
            gLastEndClock = clock;
        }
        return;
    }
    gPending = false;
    if (!gEnabled || !gMem) return;

    gTotalInstr++;
    if (gPc >= gStart && gPc < gEnd) gInRange++;

    // Reconstruct the call stack leaf-first, by the method chosen in start().
    //  * Branch-stack: emit the PRE-instruction snapshot taken in beginInstr (the
    //    shadow stack has since been mutated by this instruction's call/return hook).
    //  * DWARF: unwind here, reading caller frames from memory (untouched by the
    //    instruction) using the pre-instruction registers from beginInstr.
    // Out-of-program instructions (OS / ROM / external) are NOT dropped: their cycles
    // must be accounted so the host's totals reach ~100% and the [Kickstart]/[External]
    // buckets appear. DWARF can't unwind from an out-of-range leaf (unwindDwarf returns
    // 0), so force the raw leaf PC; the host classifies it by address range.
    u32 stack[kMaxDepth];
    u32 depth;
    if (gMethod == Method::Branch) {
        depth = gBranchDepth;
        for (u32 i = 0; i < depth; i++) stack[i] = gBranchStack[i];
    } else {
        depth = unwindDwarf(stack, kMaxDepth);
    }
    if (depth == 0) { stack[0] = gPc; depth = 1; } // out-of-range leaf, no unwind context

    // Append: [depth, pc..., cycleDelta]
    gOutput.push_back(depth);
    for (u32 i = 0; i < depth; i++) gOutput.push_back(stack[i]);
    gOutput.push_back((u32)(clock - gClock));

    // Mark where this instruction ended, so a following dispatch's no-op endInstr can
    // measure the [IRQ] gap (the cycles between here and the handler's first instruction).
    gLastEndClock = clock;
    gHaveLastEnd = true;
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
