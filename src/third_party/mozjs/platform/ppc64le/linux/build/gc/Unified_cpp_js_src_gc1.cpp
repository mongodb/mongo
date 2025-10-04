#define MOZ_UNIFIED_BUILD
#include "gc/GCAPI.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/GCAPI.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/GCAPI.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/GCParallelTask.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/GCParallelTask.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/GCParallelTask.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Heap.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Heap.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Heap.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/MallocedBlockCache.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/MallocedBlockCache.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/MallocedBlockCache.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
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