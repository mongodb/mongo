#define MOZ_UNIFIED_BUILD
#include "jit/DominatorTree.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/DominatorTree.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/DominatorTree.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/EdgeCaseAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/EdgeCaseAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/EdgeCaseAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/EffectiveAddressAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/EffectiveAddressAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/EffectiveAddressAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/ExecutableAllocator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/ExecutableAllocator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/ExecutableAllocator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/FlushICache.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/FlushICache.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/FlushICache.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/FoldLinearArithConstants.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/FoldLinearArithConstants.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/FoldLinearArithConstants.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif