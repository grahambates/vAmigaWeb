// -----------------------------------------------------------------------------
// vscode-vamiga-debugger host bridge — see HostBridge.h / FORK_NOTES.md
// -----------------------------------------------------------------------------

#include "config.h"
#include "HostBridge.h"
#include "Amiga.h"
#include "Memory.h"
#include "CPU.h"
#include "MoiraDebugger.h"

#include <string>

namespace vamiga {

namespace {

// Magic opcodes living at the trapdoor.
constexpr u16 LINEA_TRAP = 0xa00e;   // line-A opcode the guest jumps to
constexpr u16 OP_RTS     = 0x4e75;   // injected on trap so the guest call returns

// UaeLib sub-function selectors (arg0), matching WinUAE.
constexpr u32 UAELIB_CONFIG = 82;    // UaeConf(mode, index, param, ...)
constexpr u32 UAELIB_DEBUG  = 88;    // UaeLib debug commands (overlay etc.)

// Reads a NUL-terminated string from guest memory (bounded).
std::string readGuestString(Memory &mem, u32 ptr, u32 maxLen = 256)
{
    std::string s;
    for (u32 i = 0; i < maxLen; i++) {
        u8 c = mem.spypeek8<Accessor::CPU>(ptr + i);
        if (c == 0) break;
        s += char(c);
    }
    return s;
}

// UaeConf(82): apply a "<key> <value>" configuration string. Only "warp" is
// honored; the cpu_speed / *_cycle_exact lines the guest also sends are accepted
// as no-ops (vAmiga's warp subsumes them) so existing WinUAE binaries run as-is.
i32 uaeConfig(Memory &mem, Amiga &amiga, u32 paramPtr)
{
    const std::string param = readGuestString(mem, paramPtr);

    if (param == "warp true") {
        amiga.setOption(Opt::AMIGA_WARP_MODE, (i64)Warp::ALWAYS);
    } else if (param == "warp false") {
        amiga.setOption(Opt::AMIGA_WARP_MODE, (i64)Warp::NEVER);
    }
    // else: unrecognized / intentional no-op (cpu_speed, *_cycle_exact, …).

    return 0; // success
}

// Dispatch on the UaeLib sub-function (arg0). Returns the value placed in d0
// (m68k `long` / the guest's `UaeLib` return — 32-bit, matching the register).
i32 uaeLibCall(Memory &mem, Amiga &amiga, const u32 args[6])
{
    switch (args[0]) {

        case UAELIB_CONFIG:  // UaeConf(82, index, paramPtr, paramLen, outPtr, outLen)
            return uaeConfig(mem, amiga, args[2]);

        case UAELIB_DEBUG:   // Phase 2: overlay / graphics debugger — stub for now
            return 0;

        default:
            return -1;
    }
}

} // anonymous namespace

std::optional<u16> HostBridge::peek16(u32 addr)
{
    if (addr == trapAddress)     return LINEA_TRAP;  // 0xf0ff60
    if (addr == trapAddress + 2) return OP_RTS;      // 0xf0ff62
    return std::nullopt;
}

void HostBridge::install(moira::SoftwareTraps &swTraps)
{
    // Line-A 0xa00e -> inject RTS and notify via didReachSoftwareTrap (-> dispatch).
    swTraps.create(LINEA_TRAP, OP_RTS);
}

bool HostBridge::dispatch(moira::Moira &cpu, Memory &mem, Amiga &amiga, u32 addr)
{
    if (addr != trapAddress) return false;  // not our trapdoor — fall through

    // C calling convention on entry: [sp] = return address, [sp+4+4n] = argN.
    const u32 sp = cpu.getSP();
    u32 args[6];
    for (int n = 0; n < 6; n++) args[n] = mem.spypeek32<Accessor::CPU>(sp + 4 + 4 * n);

    cpu.setD(0, (u32)uaeLibCall(mem, amiga, args));  // return value in d0
    return true;  // handled inline; the injected RTS returns to the guest
}

} // namespace vamiga
