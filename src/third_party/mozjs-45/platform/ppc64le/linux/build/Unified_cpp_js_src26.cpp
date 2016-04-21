#define MOZ_UNIFIED_BUILD
#include "vm/DebuggerMemory.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/DebuggerMemory.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/DebuggerMemory.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ErrorObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ErrorObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ErrorObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ForOfIterator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ForOfIterator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ForOfIterator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/GeneratorObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/GeneratorObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/GeneratorObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/GlobalObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/GlobalObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/GlobalObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/HelperThreads.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/HelperThreads.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/HelperThreads.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif