#define MOZ_UNIFIED_BUILD
#include "jit/Ion.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Ion.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Ion.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/IonAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/IonAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/IonAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/IonBuilder.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/IonBuilder.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/IonBuilder.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/IonCaches.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/IonCaches.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/IonCaches.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/IonOptimizationLevels.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/IonOptimizationLevels.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/IonOptimizationLevels.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/JSONSpewer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JSONSpewer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JSONSpewer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/JitFrames.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JitFrames.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JitFrames.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/JitOptions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JitOptions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JitOptions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/JitSpewer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JitSpewer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JitSpewer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/JitcodeMap.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/JitcodeMap.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/JitcodeMap.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/LICM.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/LICM.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/LICM.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/LIR.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/LIR.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/LIR.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/LinearScan.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/LinearScan.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/LinearScan.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/LiveRangeAllocator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/LiveRangeAllocator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/LiveRangeAllocator.cpp defines INITGUID, so it cannot be built in unified mode."
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