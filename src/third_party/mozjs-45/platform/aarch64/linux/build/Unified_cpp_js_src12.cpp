#define MOZ_UNIFIED_BUILD
#include "jit/EffectiveAddressAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/EffectiveAddressAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/EffectiveAddressAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/ExecutableAllocator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/ExecutableAllocator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/ExecutableAllocator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/InstructionReordering.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/InstructionReordering.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/InstructionReordering.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
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