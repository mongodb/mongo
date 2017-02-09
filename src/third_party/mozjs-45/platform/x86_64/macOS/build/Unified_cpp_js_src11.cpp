#define MOZ_UNIFIED_BUILD
#include "jit/C1Spewer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/C1Spewer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/C1Spewer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/CodeGenerator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/CodeGenerator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/CodeGenerator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/CompileWrappers.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/CompileWrappers.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/CompileWrappers.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
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
