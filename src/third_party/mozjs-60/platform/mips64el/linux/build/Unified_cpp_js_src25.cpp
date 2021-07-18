#define MOZ_UNIFIED_BUILD
#include "jit/mips64/MacroAssembler-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/MacroAssembler-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/MacroAssembler-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips64/MoveEmitter-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/MoveEmitter-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/MoveEmitter-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips64/SharedIC-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/SharedIC-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/SharedIC-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips64/Trampoline-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/Trampoline-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/Trampoline-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/shared/Assembler-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/shared/Assembler-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/shared/Assembler-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/shared/BaselineCompiler-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/shared/BaselineCompiler-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/shared/BaselineCompiler-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif