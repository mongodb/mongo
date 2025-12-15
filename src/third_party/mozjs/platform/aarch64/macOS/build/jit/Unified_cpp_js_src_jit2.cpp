#define MOZ_UNIFIED_BUILD
#include "jit/BaselineJIT.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineJIT.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineJIT.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BitSet.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BitSet.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BitSet.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BranchHinting.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BranchHinting.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BranchHinting.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BytecodeAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BytecodeAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BytecodeAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/CacheIR.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/CacheIR.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/CacheIR.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/CacheIRAOT.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/CacheIRAOT.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/CacheIRAOT.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif