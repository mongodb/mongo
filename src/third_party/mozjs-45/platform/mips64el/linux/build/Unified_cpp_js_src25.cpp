#define MOZ_UNIFIED_BUILD
#include "vm/CallNonGenericMethod.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/CallNonGenericMethod.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/CallNonGenericMethod.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/CharacterEncoding.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/CharacterEncoding.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/CharacterEncoding.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/CodeCoverage.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/CodeCoverage.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/CodeCoverage.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Compression.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Compression.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Compression.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/DateTime.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/DateTime.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/DateTime.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Debugger.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Debugger.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Debugger.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif