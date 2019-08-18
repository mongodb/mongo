#define MOZ_UNIFIED_BUILD
#include "vm/Stopwatch.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Stopwatch.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Stopwatch.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/StringType.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/StringType.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/StringType.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/StructuredClone.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/StructuredClone.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/StructuredClone.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/SymbolType.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/SymbolType.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/SymbolType.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/TaggedProto.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/TaggedProto.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/TaggedProto.cpp defines INITGUID, so it cannot be built in unified mode."
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