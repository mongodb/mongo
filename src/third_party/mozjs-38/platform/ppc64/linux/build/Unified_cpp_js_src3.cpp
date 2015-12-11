#define MOZ_UNIFIED_BUILD
#include "jit/BaselineCompiler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineCompiler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineCompiler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineDebugModeOSR.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineDebugModeOSR.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineDebugModeOSR.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineFrame.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineFrame.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineFrame.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineFrameInfo.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineFrameInfo.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineFrameInfo.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineIC.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineIC.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineIC.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineInspector.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineInspector.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineInspector.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineJIT.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineJIT.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineJIT.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BitSet.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BitSet.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BitSet.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BytecodeAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BytecodeAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BytecodeAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
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