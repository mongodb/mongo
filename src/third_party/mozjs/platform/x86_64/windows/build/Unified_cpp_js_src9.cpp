#define MOZ_UNIFIED_BUILD
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
#include "irregexp/RegExpCharacters.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/RegExpCharacters.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/RegExpCharacters.cpp defines INITGUID, so it cannot be built in unified mode."
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