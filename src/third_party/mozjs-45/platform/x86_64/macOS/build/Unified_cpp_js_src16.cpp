#define MOZ_UNIFIED_BUILD
#include "jit/RangeAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/RangeAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/RangeAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/Recover.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Recover.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Recover.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/RegisterAllocator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/RegisterAllocator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/RegisterAllocator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/RematerializedFrame.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/RematerializedFrame.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/RematerializedFrame.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/Safepoints.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Safepoints.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Safepoints.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/ScalarReplacement.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/ScalarReplacement.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/ScalarReplacement.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
