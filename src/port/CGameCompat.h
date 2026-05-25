#pragma once

// CGameCompat.h — bridge to the C game headers from C++ port translation units.
//
// `global.h` (the umbrella include for the C decomp) transitively pulls in
// `functions.h`, `i3.h`, `i4.h`, and others that declare functions with a
// parameter literally named `this` — e.g. `void Zoness_ZoShrimp_Draw(Actor* this)`
// in include/i3.h. That's fine in C, but `this` is a reserved keyword in C++,
// so any C++ TU that `#include`s global.h fails to parse those declarations.
//
// We textually rename the keyword for the duration of the include. Parameter
// names don't participate in linkage — `void Foo(Actor*)` is the same symbol
// regardless of whether the parameter was spelled `this` or `self_` in the
// declaration — so the C-compiled definitions still resolve at link time.
// extern "C" wraps the include to give the function declarations C linkage,
// matching the C-compiled object files.
//
// The C++ surface is pre-included BEFORE the macro takes effect. That way
// global.h's transitive include of <libultraship.h> (which pulls in STL
// headers like <vector> whose member function bodies use `this` legitimately)
// no-ops via include guards — STL never sees the rename.
//
// Caveats:
//   - `#define` of a reserved keyword is technically UB per the C++ standard
//     (§17.4.3.1.1). MSVC, GCC, and Clang all accept it on default warning
//     levels; Clang's `-Wkeyword-macro` would flag it.
//   - The rename is scoped strictly to the include — `#undef this` restores
//     normal C++ semantics after the block, so the using TU can declare classes
//     and use `this` inside member functions without interference.
//   - If a future C-only game header reaches transitively for a new C++/STL
//     header that hasn't been pre-included here, that header's member functions
//     will see `self_` and fail to parse. Fix: add it to the pre-include block.
//
// Use this header in place of `#include "global.h"` in port-layer C++ files
// that need game-side state. See src/port/mods/Accessibility*.cpp for examples.

// Pre-include the C++ surface so STL/template headers are parsed with normal
// `this`-keyword semantics.
#include <libultraship.h>
#include "port/interpolation/FrameInterpolation.h"

#define this self_
extern "C" {
#include "global.h"
}
#undef this
