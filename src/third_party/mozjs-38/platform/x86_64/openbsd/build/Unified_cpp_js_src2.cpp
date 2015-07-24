#define MOZ_UNIFIED_BUILD
#include "gc/Statistics.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Statistics.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Statistics.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/StoreBuffer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/StoreBuffer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/StoreBuffer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Tracer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Tracer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Tracer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Verifier.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Verifier.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Verifier.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "gc/Zone.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "gc/Zone.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "gc/Zone.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/NativeRegExpMacroAssembler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/NativeRegExpMacroAssembler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/NativeRegExpMacroAssembler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/RegExpAST.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/RegExpAST.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/RegExpAST.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/RegExpEngine.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/RegExpEngine.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/RegExpEngine.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/RegExpInterpreter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/RegExpInterpreter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/RegExpInterpreter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/RegExpMacroAssembler.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/RegExpMacroAssembler.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/RegExpMacroAssembler.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/RegExpParser.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/RegExpParser.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/RegExpParser.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/RegExpStack.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/RegExpStack.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/RegExpStack.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/AliasAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/AliasAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/AliasAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BacktrackingAllocator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BacktrackingAllocator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BacktrackingAllocator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/Bailouts.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/Bailouts.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/Bailouts.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/BaselineBailouts.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/BaselineBailouts.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/BaselineBailouts.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif