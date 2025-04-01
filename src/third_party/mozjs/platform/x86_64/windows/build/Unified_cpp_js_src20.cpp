#define MOZ_UNIFIED_BUILD
#include "vm/Shape.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Shape.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Shape.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ShapeZone.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ShapeZone.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ShapeZone.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/SharedArrayObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/SharedArrayObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/SharedArrayObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/SharedImmutableStringsCache.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/SharedImmutableStringsCache.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/SharedImmutableStringsCache.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/SharedScriptDataTableHolder.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/SharedScriptDataTableHolder.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/SharedScriptDataTableHolder.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/SourceHook.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/SourceHook.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/SourceHook.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif