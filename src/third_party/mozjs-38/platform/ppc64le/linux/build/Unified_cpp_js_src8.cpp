#define MOZ_UNIFIED_BUILD
#include "perf/jsperf.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "perf/jsperf.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "perf/jsperf.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "prmjtime.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "prmjtime.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "prmjtime.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "proxy/ScriptedIndirectProxyHandler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/ScriptedIndirectProxyHandler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/ScriptedIndirectProxyHandler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/SecurityWrapper.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/SecurityWrapper.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/SecurityWrapper.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "proxy/Wrapper.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "proxy/Wrapper.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "proxy/Wrapper.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ArgumentsObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ArgumentsObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ArgumentsObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ArrayBufferObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ArrayBufferObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ArrayBufferObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/CallNonGenericMethod.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/CallNonGenericMethod.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/CallNonGenericMethod.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/CharacterEncoding.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/CharacterEncoding.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/CharacterEncoding.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Compression.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Compression.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Compression.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif