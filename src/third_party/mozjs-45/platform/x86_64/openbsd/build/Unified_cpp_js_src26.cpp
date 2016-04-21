#define MOZ_UNIFIED_BUILD
#include "proxy/CrossCompartmentWrapper.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/CrossCompartmentWrapper.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/CrossCompartmentWrapper.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/DeadObjectProxy.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/DeadObjectProxy.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/DeadObjectProxy.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/DirectProxyHandler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/DirectProxyHandler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/DirectProxyHandler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/OpaqueCrossCompartmentWrapper.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/OpaqueCrossCompartmentWrapper.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/OpaqueCrossCompartmentWrapper.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/Proxy.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/Proxy.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/Proxy.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/ScriptedDirectProxyHandler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/ScriptedDirectProxyHandler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/ScriptedDirectProxyHandler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif