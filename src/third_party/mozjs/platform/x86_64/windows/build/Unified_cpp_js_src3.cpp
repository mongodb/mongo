#define MOZ_UNIFIED_BUILD
#include "builtin/String.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/String.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/String.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/Symbol.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Symbol.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Symbol.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/TestingFunctions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/TestingFunctions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/TestingFunctions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/TestingUtility.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/TestingUtility.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/TestingUtility.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/WeakMapObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/WeakMapObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/WeakMapObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/WeakRefObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/WeakRefObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/WeakRefObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif