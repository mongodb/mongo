#define MOZ_UNIFIED_BUILD
#include "vm/Xdr.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "vm/Xdr.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "vm/Xdr.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/AsmJS.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/AsmJS.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/AsmJS.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmBaselineCompile.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmBaselineCompile.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmBaselineCompile.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmBinaryIterator.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmBinaryIterator.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmBinaryIterator.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmBinaryToAST.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmBinaryToAST.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmBinaryToAST.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "wasm/WasmBinaryToText.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "wasm/WasmBinaryToText.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "wasm/WasmBinaryToText.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif