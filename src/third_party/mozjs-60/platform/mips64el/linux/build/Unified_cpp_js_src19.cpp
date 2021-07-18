#define MOZ_UNIFIED_BUILD
#include "jit/MacroAssembler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/MacroAssembler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/MacroAssembler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/MoveResolver.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/MoveResolver.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/MoveResolver.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/OptimizationTracking.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/OptimizationTracking.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/OptimizationTracking.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/PerfSpewer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/PerfSpewer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/PerfSpewer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/ProcessExecutableMemory.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/ProcessExecutableMemory.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/ProcessExecutableMemory.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/RangeAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/RangeAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/RangeAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif