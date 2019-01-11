#define MOZ_UNIFIED_BUILD
#include "gc/Marking.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Marking.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Marking.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Memory.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Memory.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Memory.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Nursery.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Nursery.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Nursery.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "gc/Statistics.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Statistics.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Statistics.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif