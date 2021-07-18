#define MOZ_UNIFIED_BUILD
#include "jit/Linker.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Linker.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Linker.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/LoopUnroller.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/LoopUnroller.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/LoopUnroller.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/Lowering.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Lowering.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Lowering.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/MCallOptimize.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/MCallOptimize.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/MCallOptimize.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/MIR.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/MIR.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/MIR.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/MIRGraph.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/MIRGraph.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/MIRGraph.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif