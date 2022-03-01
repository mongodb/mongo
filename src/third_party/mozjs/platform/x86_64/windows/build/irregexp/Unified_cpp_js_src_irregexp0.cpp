#define MOZ_UNIFIED_BUILD
#include "irregexp/RegExpAPI.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/RegExpAPI.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/RegExpAPI.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/RegExpShim.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/RegExpShim.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/RegExpShim.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/imported/regexp-ast.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/imported/regexp-ast.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/imported/regexp-ast.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/imported/regexp-bytecode-generator.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/imported/regexp-bytecode-generator.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/imported/regexp-bytecode-generator.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/imported/regexp-bytecode-peephole.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/imported/regexp-bytecode-peephole.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/imported/regexp-bytecode-peephole.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/imported/regexp-bytecodes.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/imported/regexp-bytecodes.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/imported/regexp-bytecodes.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif