// Single translation unit that compiles miniaudio's implementation. Every other
// file includes miniaudio.h for declarations only; MINIAUDIO_IMPLEMENTATION must
// be defined in exactly one TU. Guarded by HAVE_STEAM_AUDIO so non-spatial-audio
// builds (e.g. Switch) skip it entirely.
#ifdef HAVE_STEAM_AUDIO
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#endif
