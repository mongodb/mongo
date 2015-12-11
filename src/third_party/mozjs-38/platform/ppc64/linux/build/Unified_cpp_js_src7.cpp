#define MOZ_UNIFIED_BUILD
#include "jsfriendapi.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsfriendapi.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsfriendapi.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsfun.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsfun.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsfun.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsgc.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsgc.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsgc.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsiter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsiter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsiter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsnativestack.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsnativestack.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsnativestack.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
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
#include "jsreflect.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsreflect.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsreflect.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsscript.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsscript.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsscript.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsstr.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsstr.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsstr.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jswatchpoint.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jswatchpoint.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jswatchpoint.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsweakmap.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsweakmap.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsweakmap.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif