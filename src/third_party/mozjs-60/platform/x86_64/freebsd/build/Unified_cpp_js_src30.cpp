#define MOZ_UNIFIED_BUILD
#include "util/Text.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "util/Text.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "util/Text.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "util/Unicode.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "util/Unicode.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "util/Unicode.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ArgumentsObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ArgumentsObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ArgumentsObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ArrayBufferObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ArrayBufferObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ArrayBufferObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/AsyncFunction.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/AsyncFunction.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/AsyncFunction.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/AsyncIteration.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/AsyncIteration.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/AsyncIteration.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif