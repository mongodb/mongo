#define MOZ_UNIFIED_BUILD
#include "jit/mips-shared/BaselineIC-mips-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips-shared/BaselineIC-mips-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips-shared/BaselineIC-mips-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips-shared/CodeGenerator-mips-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips-shared/CodeGenerator-mips-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips-shared/CodeGenerator-mips-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips-shared/Lowering-mips-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips-shared/Lowering-mips-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips-shared/Lowering-mips-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips-shared/MacroAssembler-mips-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips-shared/MacroAssembler-mips-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips-shared/MacroAssembler-mips-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips-shared/MoveEmitter-mips-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips-shared/MoveEmitter-mips-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips-shared/MoveEmitter-mips-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips64/Architecture-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/Architecture-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/Architecture-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif