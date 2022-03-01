#define MOZ_UNIFIED_BUILD
#include "frontend/ExpressionStatementEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/ExpressionStatementEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/ExpressionStatementEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/FoldConstants.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/FoldConstants.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/FoldConstants.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/ForInEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/ForInEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/ForInEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/ForOfEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/ForOfEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/ForOfEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/ForOfLoopControl.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/ForOfLoopControl.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/ForOfLoopControl.cpp defines INITGUID, so it cannot be built in unified mode."
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