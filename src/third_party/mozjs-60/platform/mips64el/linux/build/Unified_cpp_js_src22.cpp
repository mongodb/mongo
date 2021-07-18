#define MOZ_UNIFIED_BUILD
#include "jit/ValueNumbering.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/ValueNumbering.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/ValueNumbering.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/WasmBCE.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/WasmBCE.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/WasmBCE.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips-shared/Architecture-mips-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips-shared/Architecture-mips-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips-shared/Architecture-mips-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips-shared/Assembler-mips-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips-shared/Assembler-mips-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips-shared/Assembler-mips-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips-shared/Bailouts-mips-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips-shared/Bailouts-mips-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips-shared/Bailouts-mips-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/mips-shared/BaselineCompiler-mips-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/mips-shared/BaselineCompiler-mips-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/mips-shared/BaselineCompiler-mips-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif