#define MOZ_UNIFIED_BUILD
#include "jit/mips64/Assembler-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/Assembler-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/Assembler-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips64/Bailouts-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/Bailouts-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/Bailouts-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips64/BaselineCompiler-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/BaselineCompiler-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/BaselineCompiler-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips64/BaselineIC-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/BaselineIC-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/BaselineIC-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips64/CodeGenerator-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/CodeGenerator-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/CodeGenerator-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips64/Lowering-mips64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips64/Lowering-mips64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips64/Lowering-mips64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif