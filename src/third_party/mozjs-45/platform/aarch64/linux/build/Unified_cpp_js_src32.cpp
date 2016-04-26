#define MOZ_UNIFIED_BUILD
#include "vm/TypeInference.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/TypeInference.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/TypeInference.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/TypedArrayObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/TypedArrayObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/TypedArrayObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/UbiNode.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/UbiNode.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/UbiNode.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/UbiNodeCensus.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/UbiNodeCensus.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/UbiNodeCensus.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/UnboxedObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/UnboxedObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/UnboxedObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Unicode.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Unicode.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Unicode.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif