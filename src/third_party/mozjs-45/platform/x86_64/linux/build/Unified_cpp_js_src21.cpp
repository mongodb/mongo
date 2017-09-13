#define MOZ_UNIFIED_BUILD
#include "jit/x86-shared/BaselineIC-x86-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86-shared/BaselineIC-x86-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86-shared/BaselineIC-x86-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86-shared/CodeGenerator-x86-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86-shared/CodeGenerator-x86-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86-shared/CodeGenerator-x86-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x86-shared/Lowering-x86-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x86-shared/Lowering-x86-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x86-shared/Lowering-x86-shared.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "jsalloc.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jsalloc.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jsalloc.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif