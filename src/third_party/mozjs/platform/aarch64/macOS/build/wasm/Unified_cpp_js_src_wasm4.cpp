#define MOZ_UNIFIED_BUILD
#include "wasm/WasmMemory.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmMemory.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmMemory.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmModule.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmModule.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmModule.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmModuleTypes.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmModuleTypes.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmModuleTypes.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmOpIter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmOpIter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmOpIter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmPI.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmPI.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmPI.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmProcess.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmProcess.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmProcess.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif