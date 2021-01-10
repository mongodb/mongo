#define MOZ_UNIFIED_BUILD
#include "jit/IonControlFlow.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/IonControlFlow.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/IonControlFlow.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/IonIC.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/IonIC.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/IonIC.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/IonOptimizationLevels.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/IonOptimizationLevels.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/IonOptimizationLevels.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/JSJitFrameIter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JSJitFrameIter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JSJitFrameIter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/JSONSpewer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JSONSpewer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JSONSpewer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/Jit.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Jit.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Jit.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif