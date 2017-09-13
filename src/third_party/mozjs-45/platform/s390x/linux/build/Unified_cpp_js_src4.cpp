#define MOZ_UNIFIED_BUILD
#include "ds/LifoAlloc.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "ds/LifoAlloc.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "ds/LifoAlloc.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/BytecodeCompiler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/BytecodeCompiler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/BytecodeCompiler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/BytecodeEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/BytecodeEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/BytecodeEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/FoldConstants.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/FoldConstants.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/FoldConstants.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/NameFunctions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/NameFunctions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/NameFunctions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/ParseMaps.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/ParseMaps.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/ParseMaps.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif