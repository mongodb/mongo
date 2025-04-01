#define MOZ_UNIFIED_BUILD
#include "wasm/WasmDebugFrame.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmDebugFrame.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmDebugFrame.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmDump.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmDump.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmDump.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmFeatures.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmFeatures.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmFeatures.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmFrameIter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmFrameIter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmFrameIter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmGC.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmGC.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmGC.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmGcObject.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmGcObject.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmGcObject.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif