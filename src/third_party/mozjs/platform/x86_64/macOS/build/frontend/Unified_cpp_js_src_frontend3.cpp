#define MOZ_UNIFIED_BUILD
#include "frontend/FrontendContext.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/FrontendContext.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/FrontendContext.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/FunctionEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/FunctionEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/FunctionEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/IfEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/IfEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/IfEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/JumpList.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/JumpList.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/JumpList.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/LabelEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/LabelEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/LabelEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/LexicalScopeEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/LexicalScopeEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/LexicalScopeEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif