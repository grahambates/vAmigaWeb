// -----------------------------------------------------------------------------
// vscode-vamiga-debugger memory protection
//
// Breaks on writes to RAM outside a dynamic allow-list of ranges (the
// debugged program's own loaded segments/stack, plus anything it AllocMem's
// while running), excluding the low-memory exception vector table (always
// allowed). Mirrors the PUAE backend's e9k_debug_memprotect (see
// puae-wasm/ami_debug.c / puae-wasm/e9k/e9k_memprotect.h).
//
// This whole module is fork-local — upstream vAmiga has no file here, so it
// never causes merge conflicts. It is wired in via tagged one-line hooks
//   // [vscode-vamiga-debugger mem protect]
// in Moira.cpp, MoiraDataflow_cpp.h, Moira.h, MoiraTypes.h, AmigaTypes.h,
// Amiga.cpp and MsgQueueTypes.h.
// -----------------------------------------------------------------------------

#pragma once

#include "BasicTypes.h"

namespace vamiga {

class Amiga;
class Memory;
namespace moira { class Moira; }

namespace MemProtect {

struct Violation
{
    u32 pc;
    u32 addr;
    u32 value;
    u32 sizeBits;
};

// Enables/disables the per-write allow-list check (State::CHECK_MP).
void setEnabled(moira::Moira &cpu, bool enabled);

// Starts (or restarts) the live AllocMem/FreeMem watch that builds the
// allow-list (State::CHECK_MP_TRACK), independent of whether enforcement is
// enabled. Validates execBase (via OSDebugger's checksum check) before
// committing, so it's safe to call on every tick from the moment the
// machine starts running — it'll simply no-op (returning false) until
// exec.library has actually initialized itself, which is far earlier than
// the "user task started" heuristic used elsewhere (see vamiga_app.js's
// tryExec). Starting this early means Kickstart's own boot-time AllocMem
// calls (graphics.library's default View/copper lists, etc.) get tracked
// too, not just whatever a user task allocates afterwards — otherwise
// legitimate writes into those structures (e.g. a user program's LoadView
// call) would be misflagged as violations.
//
// Callers must stop polling once this first returns true — calling it
// again while already tracking discards any AllocMem call currently
// in-flight (see instrHook's s_allocPending). Safe to call again
// deliberately after an explicit reset/reboot to recompute the AllocMem/
// FreeMem LVO addresses from the new execBase.
bool startTracking(moira::Moira &cpu, Amiga &amiga);

// Adds every library currently on ExecBase->LibList, plus a budget below
// the supervisor stack pointer (see the .cpp for why on both). Deliberately
// separate from startTracking(): execBase's own checksum (which
// startTracking validates) only covers a small field range and says
// nothing about whether LibList itself has been initialized yet, so
// calling this as early as startTracking() succeeds risks walking
// uninitialized garbage as if it were a real list. Call this instead once
// the caller already trusts library state is live — e.g. the same
// "GfxBase is set" condition vamiga_app.js's tryExec already uses to gate
// other graphics-state reads. Safe to call repeatedly (e.g. after a reset);
// each call just re-adds whatever's currently resident/current. Returns
// false without adding anything if execBase doesn't validate at the time
// of the call.
bool seedResidentLibraries(Amiga &amiga);

void resetRanges();
int addRange(u32 addr, u32 size);

// Called on every instruction when State::CHECK_MP_TRACK is set. Tracks
// AllocMem/FreeMem calls (system-wide) to keep the allow-list in sync with
// what's actually allocated.
void instrHook(moira::Moira &cpu, Memory &mem, u32 pc);

// Called on every write when State::CHECK_MP is set. `size` is the access
// size in bytes (1/2/4 — Moira's Byte/Word/Long).
void checkWrite(moira::Moira &cpu, Memory &mem, Amiga &amiga, u32 addr, u32 value, int size);

// Retrieves the most recently recorded violation (valid only when the
// caller is responding to RL::MEMPROTECT_VIOLATION_REACHED).
Violation lastViolation();

}
}
