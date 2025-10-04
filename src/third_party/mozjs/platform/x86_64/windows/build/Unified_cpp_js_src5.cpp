#define MOZ_UNIFIED_BUILD
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
#include "jsnum.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsnum.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsnum.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/BaseProxyHandler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/BaseProxyHandler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/BaseProxyHandler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/CrossCompartmentWrapper.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/CrossCompartmentWrapper.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/CrossCompartmentWrapper.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/DOMProxy.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/DOMProxy.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/DOMProxy.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif