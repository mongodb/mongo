#define MOZ_UNIFIED_BUILD
#include "gc/Zone.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Zone.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Zone.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif