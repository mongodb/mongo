#define MOZ_UNIFIED_BUILD
#include "jit/WarpOracle.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/WarpOracle.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/WarpOracle.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/WarpSnapshot.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/WarpSnapshot.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/WarpSnapshot.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "jit/XrayJitInfo.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/XrayJitInfo.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/XrayJitInfo.cpp defines INITGUID, so it cannot be built in unified mode."
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
#include "jit/shared/Assembler-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/shared/Assembler-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/shared/Assembler-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif