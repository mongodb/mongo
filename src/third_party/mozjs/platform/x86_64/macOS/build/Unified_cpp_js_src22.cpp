#define MOZ_UNIFIED_BUILD
#include "vm/TaggedProto.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/TaggedProto.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/TaggedProto.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ThrowMsgKind.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ThrowMsgKind.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ThrowMsgKind.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Time.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Time.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Time.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/ToSource.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ToSource.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ToSource.cpp defines INITGUID, so it cannot be built in unified mode."
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