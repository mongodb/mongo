#define MOZ_UNIFIED_BUILD
#include "vm/ObjectWithStashedPointer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/ObjectWithStashedPointer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/ObjectWithStashedPointer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/OffThreadPromiseRuntimeState.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/OffThreadPromiseRuntimeState.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/OffThreadPromiseRuntimeState.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/PlainObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/PlainObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/PlainObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Prefs.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Prefs.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Prefs.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Printer.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Printer.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Printer.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Probes.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Probes.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Probes.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif