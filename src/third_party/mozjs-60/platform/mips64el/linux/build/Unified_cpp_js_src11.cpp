#define MOZ_UNIFIED_BUILD
#include "jit/Bailouts.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Bailouts.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Bailouts.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineBailouts.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineBailouts.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineBailouts.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineCacheIRCompiler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineCacheIRCompiler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineCacheIRCompiler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineCompiler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineCompiler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineCompiler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineDebugModeOSR.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineDebugModeOSR.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineDebugModeOSR.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineFrame.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineFrame.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineFrame.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif