#define MOZ_UNIFIED_BUILD
#include "frontend/CForEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/CForEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/CForEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/CallOrNewEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/CallOrNewEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/CallOrNewEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/DefaultEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/DefaultEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/DefaultEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/DoWhileEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/DoWhileEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/DoWhileEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/ElemOpEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/ElemOpEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/ElemOpEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/EmitterScope.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/EmitterScope.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/EmitterScope.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif