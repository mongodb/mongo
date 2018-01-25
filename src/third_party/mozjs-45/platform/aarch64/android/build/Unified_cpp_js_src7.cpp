#define MOZ_UNIFIED_BUILD
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