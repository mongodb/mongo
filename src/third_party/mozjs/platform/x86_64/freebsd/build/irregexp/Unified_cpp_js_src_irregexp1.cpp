#define MOZ_UNIFIED_BUILD
#include "irregexp/imported/regexp-compiler-tonode.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/imported/regexp-compiler-tonode.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/imported/regexp-compiler-tonode.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/imported/regexp-dotprinter.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/imported/regexp-dotprinter.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/imported/regexp-dotprinter.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/imported/regexp-interpreter.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/imported/regexp-interpreter.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/imported/regexp-interpreter.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/imported/regexp-macro-assembler-tracer.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/imported/regexp-macro-assembler-tracer.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/imported/regexp-macro-assembler-tracer.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/imported/regexp-macro-assembler.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/imported/regexp-macro-assembler.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/imported/regexp-macro-assembler.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "irregexp/imported/regexp-parser.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "irregexp/imported/regexp-parser.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "irregexp/imported/regexp-parser.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif