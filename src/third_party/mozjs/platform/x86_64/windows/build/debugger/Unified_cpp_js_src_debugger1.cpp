#define MOZ_UNIFIED_BUILD
#include "debugger/Object.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "debugger/Object.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "debugger/Object.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "debugger/Script.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "debugger/Script.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "debugger/Script.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "debugger/Source.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "debugger/Source.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "debugger/Source.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif