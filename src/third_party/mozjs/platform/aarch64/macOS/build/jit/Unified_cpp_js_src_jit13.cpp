#define MOZ_UNIFIED_BUILD
#include "jit/arm64/Lowering-arm64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm64/Lowering-arm64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm64/Lowering-arm64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm64/MacroAssembler-arm64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm64/MacroAssembler-arm64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm64/MacroAssembler-arm64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm64/MoveEmitter-arm64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm64/MoveEmitter-arm64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm64/MoveEmitter-arm64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm64/Trampoline-arm64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm64/Trampoline-arm64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm64/Trampoline-arm64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm64/vixl/Assembler-vixl.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm64/vixl/Assembler-vixl.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm64/vixl/Assembler-vixl.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm64/vixl/Cpu-Features-vixl.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm64/vixl/Cpu-Features-vixl.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm64/vixl/Cpu-Features-vixl.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif