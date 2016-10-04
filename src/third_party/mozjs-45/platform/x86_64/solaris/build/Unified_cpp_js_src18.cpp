#define MOZ_UNIFIED_BUILD
#include "jit/VMFunctions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/VMFunctions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/VMFunctions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/ValueNumbering.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/ValueNumbering.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/ValueNumbering.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/none/Trampoline-none.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/none/Trampoline-none.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/none/Trampoline-none.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "jit/shared/CodeGenerator-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/shared/CodeGenerator-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/shared/CodeGenerator-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/shared/Lowering-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/shared/Lowering-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/shared/Lowering-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
