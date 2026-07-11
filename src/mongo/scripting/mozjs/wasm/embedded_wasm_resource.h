// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include "mongo/util/assert_util.h"

namespace mongo::mozjs::wasm {

#ifdef _WIN32

/**
 * Returns {pointer, size} for a Windows resource embedded with embed_binary_rc.
 * The resource name must match the resource_name attribute in the BUILD rule.
 */
inline std::pair<const uint8_t*, size_t> getEmbeddedWasmResource() {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, L"MOZJS_WASM_CWASM", RT_RCDATA);
    invariant(res, "MOZJS_WASM_CWASM resource not found in binary");
    HGLOBAL resHandle = LoadResource(mod, res);
    invariant(resHandle, "Failed to load MOZJS_WASM_CWASM resource");
    const uint8_t* data = static_cast<const uint8_t*>(LockResource(resHandle));
    invariant(data, "Failed to lock MOZJS_WASM_CWASM resource");
    size_t size = SizeofResource(mod, res);
    invariant(size > 0, "MOZJS_WASM_CWASM resource has zero size");
    return {data, size};
}

#else

extern "C" {
extern const uint8_t _binary_mozjs_wasm_api_cwasm_start[];
extern const uint8_t _binary_mozjs_wasm_api_cwasm_end[];
}

inline std::pair<const uint8_t*, size_t> getEmbeddedWasmResource() {
    const uint8_t* data = _binary_mozjs_wasm_api_cwasm_start;
    size_t size =
        static_cast<size_t>(_binary_mozjs_wasm_api_cwasm_end - _binary_mozjs_wasm_api_cwasm_start);
    return {data, size};
}

#endif

}  // namespace mongo::mozjs::wasm
