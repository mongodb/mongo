#define MOZ_UNIFIED_BUILD
#include "jspropertytree.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jspropertytree.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jspropertytree.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsscript.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsscript.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsscript.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsstr.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsstr.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsstr.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jswatchpoint.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jswatchpoint.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jswatchpoint.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jsweakmap.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsweakmap.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsweakmap.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "perf/jsperf.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "perf/jsperf.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "perf/jsperf.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif