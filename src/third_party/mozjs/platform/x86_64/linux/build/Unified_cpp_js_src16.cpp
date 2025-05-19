#define MOZ_UNIFIED_BUILD
#include "vm/JitActivation.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/JitActivation.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/JitActivation.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/List.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/List.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/List.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/MemoryMetrics.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/MemoryMetrics.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/MemoryMetrics.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/Modules.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Modules.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Modules.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "vm/NativeObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/NativeObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/NativeObject.cpp defines INITGUID, so it cannot be built in unified mode."
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