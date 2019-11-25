#define MOZ_UNIFIED_BUILD
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
#include "jit/AlignmentMaskAnalysis.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/AlignmentMaskAnalysis.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/AlignmentMaskAnalysis.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif