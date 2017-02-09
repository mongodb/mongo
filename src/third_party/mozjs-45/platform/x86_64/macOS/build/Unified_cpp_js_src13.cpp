#define MOZ_UNIFIED_BUILD
#include "jit/IonCaches.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/IonCaches.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/IonCaches.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "jit/JSONSpewer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JSONSpewer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JSONSpewer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/JitFrames.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JitFrames.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JitFrames.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/JitOptions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JitOptions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JitOptions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/JitSpewer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JitSpewer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JitSpewer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
