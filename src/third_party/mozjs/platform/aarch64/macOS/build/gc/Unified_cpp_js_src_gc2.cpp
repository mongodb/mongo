#define MOZ_UNIFIED_BUILD
#include "gc/Nursery.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Nursery.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Nursery.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/ParallelMarking.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/ParallelMarking.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/ParallelMarking.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Pretenuring.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Pretenuring.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Pretenuring.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/PublicIterators.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/PublicIterators.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/PublicIterators.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/RootMarking.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/RootMarking.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/RootMarking.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Scheduling.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Scheduling.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Scheduling.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif