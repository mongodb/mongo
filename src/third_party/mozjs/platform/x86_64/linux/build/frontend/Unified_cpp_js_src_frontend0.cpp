#define MOZ_UNIFIED_BUILD
#include "frontend/AbstractScopePtr.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/AbstractScopePtr.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/AbstractScopePtr.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/AsyncEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/AsyncEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/AsyncEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/BytecodeCompiler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/BytecodeCompiler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/BytecodeCompiler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/BytecodeControlStructures.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/BytecodeControlStructures.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/BytecodeControlStructures.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/BytecodeEmitter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/BytecodeEmitter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/BytecodeEmitter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "frontend/BytecodeSection.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "frontend/BytecodeSection.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "frontend/BytecodeSection.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif