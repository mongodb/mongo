#define MOZ_UNIFIED_BUILD
#include "jit/ShuffleAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/ShuffleAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/ShuffleAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/Sink.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Sink.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Sink.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/Snapshots.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Snapshots.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Snapshots.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/Trampoline.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Trampoline.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Trampoline.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/TrampolineNatives.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/TrampolineNatives.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/TrampolineNatives.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/TrialInlining.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/TrialInlining.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/TrialInlining.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif