#define MOZ_UNIFIED_BUILD
#include "frontend/TryEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/TryEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/TryEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/WhileEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/WhileEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/WhileEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif