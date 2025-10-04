#define MOZ_UNIFIED_BUILD
#include "builtin/FinalizationRegistryObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/FinalizationRegistryObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/FinalizationRegistryObject.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "builtin/Object.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Object.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Object.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/ParseRecordObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/ParseRecordObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/ParseRecordObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif