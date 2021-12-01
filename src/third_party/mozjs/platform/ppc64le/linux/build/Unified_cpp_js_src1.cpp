#define MOZ_UNIFIED_BUILD
#include "builtin/Object.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Object.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Object.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/Profilers.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Profilers.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Profilers.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/Promise.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Promise.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Promise.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/Reflect.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/Reflect.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/Reflect.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/ReflectParse.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/ReflectParse.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/ReflectParse.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "builtin/SIMD.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "builtin/SIMD.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "builtin/SIMD.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif