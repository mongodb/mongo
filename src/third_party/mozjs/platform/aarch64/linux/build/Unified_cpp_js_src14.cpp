#define MOZ_UNIFIED_BUILD
#include "vm/Initialization.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Initialization.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Initialization.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/InternalThreadPool.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/InternalThreadPool.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/InternalThreadPool.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/InvalidatingFuse.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/InvalidatingFuse.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/InvalidatingFuse.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Iteration.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Iteration.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Iteration.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Iterator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Iterator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Iterator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/JSAtomUtils.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/JSAtomUtils.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/JSAtomUtils.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif