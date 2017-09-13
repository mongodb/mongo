#define MOZ_UNIFIED_BUILD
#include "jit/x64/Bailouts-x64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x64/Bailouts-x64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x64/Bailouts-x64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x64/BaselineCompiler-x64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x64/BaselineCompiler-x64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x64/BaselineCompiler-x64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x64/BaselineIC-x64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x64/BaselineIC-x64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x64/BaselineIC-x64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x64/CodeGenerator-x64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x64/CodeGenerator-x64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x64/CodeGenerator-x64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x64/Lowering-x64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x64/Lowering-x64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x64/Lowering-x64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/x64/MacroAssembler-x64.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/x64/MacroAssembler-x64.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/x64/MacroAssembler-x64.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif