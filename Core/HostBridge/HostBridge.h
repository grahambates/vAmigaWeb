// -----------------------------------------------------------------------------
// vscode-vamiga-debugger host bridge
//
// A WinUAE-compatible "UaeLib" trapdoor at 0xf0ff60. The guest calls it like a
// function pointer; a line-A opcode (0xa00e) at that address springs an inline
// software trap that the host services (warp control now; debug overlay and a
// graphics debugger later). Binary-compatible with existing WinUAE debug code.
//
// This whole module is fork-local — upstream vAmiga has no file here, so it
// never causes merge conflicts. It is wired in via three one-line hooks tagged
//   // [vscode-vamiga-debugger host bridge]
// in Memory.cpp and CPU.cpp. See FORK_NOTES.md for the exact sites.
// -----------------------------------------------------------------------------

#pragma once

#include "BasicTypes.h"
#include <optional>

namespace vamiga {

class Amiga;
class Memory;
namespace moira { class Moira; struct SoftwareTraps; }

namespace HostBridge {

// Trapdoor entry point — matches WinUAE's uaelib address for binary compatibility.
constexpr u32 trapAddress = 0xf0ff60;

// Synthesizes the trapdoor bytes for an otherwise-unmapped read:
//   0xf0ff60 -> 0xa00e (line-A trap opcode — what the guest's presence check reads)
//   0xf0ff62 -> 0x4e75 (RTS; cosmetic — the trap injects its own RTS at runtime)
// Returns nullopt for every other address. Invoked only from the unmapped-read
// path, so it never shadows real RAM/ROM (e.g. a loaded CDTV/CD32 ext-ROM).
std::optional<u16> peek16(u32 addr);

// Registers the line-A software trap (0xa00e -> RTS injection). Call once at reset.
void install(moira::SoftwareTraps &swTraps);

// Services a software-trap hit. Returns true if `addr` is our trapdoor (handled
// inline, no CPU halt); false to let the normal software-trap path run.
bool dispatch(moira::Moira &cpu, Memory &mem, Amiga &amiga, u32 addr);

}
}
