#define MOZ_UNIFIED_BUILD
#include "jit/Disassembler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Disassembler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Disassembler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/EagerSimdUnbox.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/EagerSimdUnbox.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/EagerSimdUnbox.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/EdgeCaseAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/EdgeCaseAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/EdgeCaseAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
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
#include "jit/FlowAliasAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/FlowAliasAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/FlowAliasAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif