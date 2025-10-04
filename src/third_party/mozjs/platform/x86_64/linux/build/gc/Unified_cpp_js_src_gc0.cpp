#define MOZ_UNIFIED_BUILD
#include "gc/Allocator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Allocator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Allocator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/AtomMarking.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/AtomMarking.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/AtomMarking.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Barrier.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Barrier.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Barrier.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Compacting.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Compacting.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Compacting.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/FinalizationObservers.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/FinalizationObservers.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/FinalizationObservers.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/GC.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/GC.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/GC.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif