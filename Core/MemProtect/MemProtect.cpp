// -----------------------------------------------------------------------------
// vscode-vamiga-debugger memory protection — see MemProtect.h
// -----------------------------------------------------------------------------

#include "config.h"
#include "MemProtect.h"
#include "Amiga.h"
#include "AmigaTypes.h"
#include "Memory.h"
#include "Moira.h"

#include <cstring>

namespace vamiga {
namespace MemProtect {

namespace {

constexpr size_t RANGE_COUNT = 128;
constexpr u32 VECTOR_TABLE_END = 0x400;

struct Range { u32 addr; u32 size; };

bool s_enabled = false;

Range s_ranges[RANGE_COUNT];
size_t s_rangeCount = 0;

u32 s_allocMemAddr = 0;
u32 s_freeMemAddr = 0;
bool s_allocPending = false;
u32 s_allocSize = 0;
u32 s_allocReturnPc = 0;

Violation s_lastViolation = {};

// Validates the ExecBase structure at `addr` using the same checksum
// AmigaOS itself relies on (ChkBase == ~addr, and the words in [0x22, 0x52]
// sum to 0xFFFF). Deliberately uses only raw spypeek calls rather than
// OSDebugger::getExecBase() — that helper declares an uninitialized
// os::ExecBase and only populates it if its own (looser) isValidPtr check
// passes, but always runs its checksum check on the result regardless,
// so calling it speculatively this early (before exec.library has written
// anything) can validate uninitialized stack garbage as if it were real.
bool execBaseValid(Memory &mem, u32 addr)
{
    if (addr == 0 || (addr & 1u) != 0) {
        return false;
    }
    if (~mem.spypeek32<Accessor::CPU>(addr + 0x26u) != addr) { // ChkBase
        return false;
    }
    u16 checksum = 0;
    for (u32 offset = 0x22u; offset <= 0x52u; offset += 2u) {
        checksum = (u16)(checksum + mem.spypeek16<Accessor::CPU>(addr + offset));
    }
    return checksum == 0xFFFFu;
}

// Adds every library currently on ExecBase->LibList to the allow-list, as
// [base - NegSize, base + PosSize] (the Library struct's documented data
// bounds — see exec/libraries.h). Library bases (GfxBase, IntuitionBase,
// DosBase, exec.library itself, ...) are bootstrapped by Kickstart before
// exec.library ever makes a single trackable AllocMem call — there's no
// LVO call for instrHook to observe, so no amount of "start tracking
// earlier" can ever see them via the dynamic watch. Writes into their own
// fields (e.g. graphics.library's LoadView updating GfxBase->ActiView/
// copper list pointers) need this one-time snapshot instead.
//
// List traversal mirrors the standard Exec idiom: ln_Succ of the last real
// node points at the list's own dummy tail node (whose ln_Succ is always
// 0), so stopping as soon as a node's ln_Succ reads 0 naturally excludes
// the tail without needing its address. Every node is sanity-checked
// (even, in RAM/ROM, plausible size) before being trusted, and traversal
// stops at the first sign of a corrupt/uninitialized list rather than
// continuing — execBase's own checksum (see execBaseValid) only covers a
// small field range and says nothing about whether LibList itself has
// been initialized yet, so without this a still-zeroed or garbage list
// could be walked as if it were real, burning through the whole range
// table on bogus entries before the caller's own ranges ever get added.
void addResidentLibraries(Memory &mem, u32 execBase)
{
    constexpr u32 LIB_LIST_OFFSET = 378;   // ExecBase->LibList
    constexpr int MAX_LIBRARIES = 128;     // generous — real systems have a few dozen
    constexpr u32 MAX_LIB_SIZE = 0x100000; // 1MB — real libraries are tiny by comparison

    u32 node = mem.spypeek32<Accessor::CPU>(execBase + LIB_LIST_OFFSET);
    for (int i = 0; i < MAX_LIBRARIES && node != 0; i++) {
        if ((node & 1u) != 0 || !(mem.inRam(node) || mem.inRom(node))) break;

        u32 succ = mem.spypeek32<Accessor::CPU>(node); // ln_Succ
        if (succ == 0) break; // `node` is the dummy tail itself
        if ((succ & 1u) != 0) break; // next link doesn't look like a real pointer

        u16 negSize = mem.spypeek16<Accessor::CPU>(node + 16); // lib_NegSize
        u16 posSize = mem.spypeek16<Accessor::CPU>(node + 18); // lib_PosSize
        u32 size = (u32)negSize + (u32)posSize;
        if (size > 0 && size < MAX_LIB_SIZE && negSize <= node) {
            addRange(node - negSize, size);
        }

        node = succ;
    }
}

// Adds a budget below the supervisor stack pointer (ISP) to the allow-list.
// Interrupt handlers, exception entry, and TRAP'd OS calls all run in
// supervisor mode and push onto this stack — it's OS-managed, never
// AllocMem'd by anyone, so without this every legitimate supervisor-mode
// push (including the CPU's own automatic exception-frame push, and a
// user-installed interrupt handler preserving registers) would falsely
// violate. Mirrors the fixed-budget approximation used for the program's
// own user-mode stack (see amigaHunkLoader.ts's STACK_RESERVE_SIZE) —
// deliberately generous, not a precise overflow boundary. Unlike
// exempting supervisor mode outright (the previous approach here), this
// still lets a wild write made *from* supervisor-mode code (e.g. a buggy
// interrupt handler writing somewhere unrelated) be caught, since only
// this specific stack region is allowed, not supervisor mode as a whole.
void addSupervisorStack(moira::Moira &cpu)
{
    constexpr u32 SUPERVISOR_STACK_BUDGET = 16 * 1024;

    u32 isp = cpu.getISP();
    if (isp == 0) return;

    addRange(isp - SUPERVISOR_STACK_BUDGET, SUPERVISOR_STACK_BUDGET + 8);
}

} // namespace

void setEnabled(moira::Moira &cpu, bool enabled)
{
    s_enabled = enabled;
    cpu.setCheckMemProtect(enabled);
}

bool startTracking(moira::Moira &cpu, Amiga &amiga)
{
    u32 execBase = amiga.mem.spypeek32<Accessor::CPU>(4);
    if (!execBaseValid(amiga.mem, execBase)) {
        return false;
    }
    s_allocMemAddr = execBase - 198; // _LVOAllocMem
    s_freeMemAddr = execBase - 210;  // _LVOFreeMem
    s_allocPending = false;
    cpu.setCheckMemProtectTracking(true);
    return true;
}

bool seedResidentLibraries(Amiga &amiga)
{
    u32 execBase = amiga.mem.spypeek32<Accessor::CPU>(4);
    if (!execBaseValid(amiga.mem, execBase)) {
        return false;
    }
    addResidentLibraries(amiga.mem, execBase);
    addSupervisorStack(amiga.cpu);
    return true;
}

void resetRanges()
{
    s_rangeCount = 0;
    s_allocPending = false;
}

int addRange(u32 addr, u32 size)
{
    if (s_rangeCount >= RANGE_COUNT) {
        return -1;
    }
    int index = (int)s_rangeCount;
    s_ranges[index].addr = addr;
    s_ranges[index].size = size;
    s_rangeCount++;
    return index;
}

void instrHook(moira::Moira &cpu, Memory &mem, u32 pc)
{
    if (s_allocPending && pc == s_allocReturnPc) {

        s_allocPending = false;
        u32 result = cpu.getD(0);
        if (result != 0 && s_allocSize != 0) {
            addRange(result, s_allocSize);
        }
    }

    if (s_allocMemAddr != 0 && pc == s_allocMemAddr) {

        // AllocMem(size=D0, attributes=D1) — capture the call args and the
        // return address (top of stack on entry) so we can pick up the
        // result (D0) once the call returns.
        s_allocSize = cpu.getD(0);
        s_allocReturnPc = mem.spypeek32<Accessor::CPU>(cpu.getSP());
        s_allocPending = true;

    } else if (s_freeMemAddr != 0 && pc == s_freeMemAddr) {

        // FreeMem(memoryBlock=A1, byteSize=D0)
        u32 freedAddr = cpu.getA(1);
        for (size_t i = 0; i < s_rangeCount; i++) {
            if (s_ranges[i].addr == freedAddr) {
                size_t remain = s_rangeCount - (i + 1);
                if (remain) {
                    memmove(&s_ranges[i], &s_ranges[i + 1], remain * sizeof(s_ranges[0]));
                }
                s_rangeCount--;
                break;
            }
        }
    }
}

void checkWrite(moira::Moira &cpu, Memory &mem, Amiga &amiga, u32 addr, u32 value, int size)
{
    if (!s_enabled) return;
    if (addr < VECTOR_TABLE_END) return;

    // Only RAM writes are subject to the allow-list — custom chip registers,
    // CIAs, ROM, etc. are addressed directly by the running program and
    // aren't (and can't be) part of an AllocMem-built range.
    if (!mem.inRam(addr)) return;

    for (size_t i = 0; i < s_rangeCount; i++) {
        u32 start = s_ranges[i].addr;
        u32 end = start + s_ranges[i].size;
        if (addr >= start && addr + (u32)size <= end) return;
    }

    s_lastViolation = { cpu.getPC0(), addr, value, (u32)(size * 8) };
    amiga.setFlag(RL::MEMPROTECT_VIOLATION_REACHED);
}

Violation lastViolation()
{
    return s_lastViolation;
}

} // namespace MemProtect
} // namespace vamiga
