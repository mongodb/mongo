#define MOZ_UNIFIED_BUILD
#include "jsnum.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsnum.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsnum.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsobj.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsobj.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsobj.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "json.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "json.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "json.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsopcode.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsopcode.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsopcode.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsprf.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsprf.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsprf.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jspropertytree.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jspropertytree.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jspropertytree.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
