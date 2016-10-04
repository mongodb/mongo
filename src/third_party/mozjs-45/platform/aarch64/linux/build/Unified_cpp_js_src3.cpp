#define MOZ_UNIFIED_BUILD
#include "builtin/SymbolObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/SymbolObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/SymbolObject.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "builtin/TypedObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/TypedObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/TypedObject.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "builtin/WeakSetObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/WeakSetObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/WeakSetObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "devtools/sharkctl.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "devtools/sharkctl.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "devtools/sharkctl.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif