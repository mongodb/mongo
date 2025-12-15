#define MOZ_UNIFIED_BUILD
#include "jit/UnrollLoops.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/UnrollLoops.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/UnrollLoops.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/VMFunctions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/VMFunctions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/VMFunctions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/ValueNumbering.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/ValueNumbering.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/ValueNumbering.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/WarpBuilder.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/WarpBuilder.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/WarpBuilder.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/WarpBuilderShared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/WarpBuilderShared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/WarpBuilderShared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/WarpCacheIRTranspiler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/WarpCacheIRTranspiler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/WarpCacheIRTranspiler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif