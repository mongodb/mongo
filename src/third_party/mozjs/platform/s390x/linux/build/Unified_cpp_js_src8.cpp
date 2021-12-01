#define MOZ_UNIFIED_BUILD
#include "gc/Tracer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Tracer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Tracer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Verifier.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Verifier.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Verifier.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/WeakMap.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/WeakMap.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/WeakMap.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/WeakMapPtr.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/WeakMapPtr.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/WeakMapPtr.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Zone.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Zone.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Zone.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/ZoneGroup.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/ZoneGroup.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/ZoneGroup.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif