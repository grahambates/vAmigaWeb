// -----------------------------------------------------------------------------
// vscode-vamiga-debugger cpu profiler
//
// A per-instruction CPU profiler ported from the vscode-amiga-debug / WinUAE
// "cpu_profiler". While enabled it records, for every executed instruction in the
// loaded program's text range, the reconstructed call stack and the elapsed cycle
// count. The host (VS Code extension) symbolicates and aggregates the flat output
// into a call tree / flame graph.
//
// Two call-stack reconstruction methods, selected automatically in start():
//   * DWARF unwind (primary, C/C++): walks A5/A7 with the uploaded per-location
//     {cfa,r13,ra} table. Used when a non-empty unwind table was uploaded; also
//     yields inlined frames (host-side).
//   * Runtime branch-stack (fallback, assembly/hunk with no DWARF .debug_frame):
//     a shadow call stack maintained by JSR/BSR (push) / RTS/RTE (pop) /
//     exception-entry hooks in Moira, ported from WinUAE's debugmem branch-stack
//     (two stacks keyed on the S-bit; pop matched by return PC). Used when the
//     uploaded unwind table is empty. See BranchStack below and FORK_NOTES.md.
// The emitted stream format is identical for both, so the host is unaware which
// produced it.
//
// This whole module is fork-local — upstream vAmiga has no file here, so it never
// causes merge conflicts. It is wired in via a few one-line hooks tagged
//   // [vscode-vamiga-debugger cpu profiler]
// in Moira.cpp / MoiraExec_cpp.h / MoiraExceptions_cpp.h / MoiraTypes.h /
// CPU.{h,cpp} / main.cpp. See FORK_NOTES.md.
// -----------------------------------------------------------------------------

#pragma once

#include "BasicTypes.h"

namespace vamiga {

class Memory;

namespace CpuProfiler {

// Synthetic leaf PC emitted for an [IRQ] sample: the cycle "gap" between one profiled
// instruction and the next, which is the interrupt/exception dispatch overhead (that
// dispatch skips beginInstr via checkForIrq -> goto done). A reserved value above any
// real Amiga address; the host (src/profilerManager.ts) classifies it as "[IRQ]". Out-
// of-program leaves keep their real PC and are classified host-side as [Kickstart]
// (ROM range) or [External]. Mirrors WinUAE's 0x7fff'ffff IRQ marker. KEEP IN SYNC with
// the IRQ_MARKER constant in src/profilerManager.ts.
constexpr u32 IRQ_MARKER = 0xFFFFFFFE;

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
// Moira PROFILING flag gates whether the per-instruction hooks below fire. startClock
// is the CPU clock at the capture's frame boundary — it seeds the [IRQ] gap tracker so
// an interrupt dispatched before the first profiled instruction is still attributed.
void start(i64 startClock);
void stop();

// Per-instruction hooks, called from Moira::execute() (slow path) only when the
// PROFILING flag is set. beginInstr stashes the pre-execution PC/A5/A7 + clock and
// the supervisor bit (which branch-stack the sample belongs to); endInstr computes
// the cycle delta, reconstructs the call stack (DWARF or branch-stack), and appends
// a record.
void beginInstr(u32 pc, u32 a5, u32 a7, u32 usp, bool super, i64 clock);
void endInstr(i64 clock);

// Runtime branch-stack hooks, called from Moira's call/return/exception paths
// (MoiraExec_cpp.h, MoiraExceptions_cpp.h) only when the PROFILING flag is set.
// No-ops unless the active method is branch-stack (empty unwind table). Ported
// 1:1 from WinUAE's debugmem branch_stack_push / _pop_rts / _pop_rte:
//   push          = branch_stack_push      (JSR/BSR; `super` = S-bit at the call)
//   popRts        = branch_stack_pop_rts   (RTS; `super` = S-bit at the return)
//   popRte        = branch_stack_pop_rte   (RTE; always unwinds the super stack)
//   enterException = branch_stack_push on the supervisor stack (exception/IRQ entry)
// `returnPC` is the address the matching RTS/RTE will return to (WinUAE next_pc).
namespace BranchStack {
    void push(bool super, u32 returnPC, u32 a7AtCall);
    void popRts(bool super, u32 returnPC);
    void popRte(u32 returnPC);
    void enterException(u32 returnPC, u32 a7);
}

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
