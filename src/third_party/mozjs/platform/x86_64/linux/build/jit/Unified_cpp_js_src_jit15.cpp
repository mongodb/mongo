#define MOZ_UNIFIED_BUILD
#include "jit/x86-shared/MacroAssembler-x86-shared-SIMD.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86-shared/MacroAssembler-x86-shared-SIMD.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86-shared/MacroAssembler-x86-shared-SIMD.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86-shared/MacroAssembler-x86-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86-shared/MacroAssembler-x86-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86-shared/MacroAssembler-x86-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86-shared/MoveEmitter-x86-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86-shared/MoveEmitter-x86-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86-shared/MoveEmitter-x86-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif