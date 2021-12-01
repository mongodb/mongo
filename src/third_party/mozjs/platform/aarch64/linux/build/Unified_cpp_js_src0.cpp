#define MOZ_UNIFIED_BUILD
#include "builtin/AtomicsObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/AtomicsObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/AtomicsObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/DataViewObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/DataViewObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/DataViewObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/Eval.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Eval.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Eval.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/JSON.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/JSON.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/JSON.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/MapObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/MapObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/MapObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/ModuleObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/ModuleObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/ModuleObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif