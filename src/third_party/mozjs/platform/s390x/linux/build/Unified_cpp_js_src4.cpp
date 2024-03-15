#define MOZ_UNIFIED_BUILD
#include "ds/Bitmap.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "ds/Bitmap.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "ds/Bitmap.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "ds/LifoAlloc.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "ds/LifoAlloc.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "ds/LifoAlloc.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsapi.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsapi.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsapi.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsdate.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsdate.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsdate.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsexn.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsexn.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsexn.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsfriendapi.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsfriendapi.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsfriendapi.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif