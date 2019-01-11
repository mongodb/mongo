#define MOZ_UNIFIED_BUILD
#include "wasm/WasmStubs.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmStubs.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmStubs.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmTable.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmTable.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmTable.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmTextToBinary.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmTextToBinary.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmTextToBinary.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmTextUtils.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmTextUtils.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmTextUtils.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmTypes.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmTypes.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmTypes.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmValidate.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmValidate.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmValidate.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif