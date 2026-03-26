/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mongo::mozjs::wasm {

#ifdef _WIN32

/**
 * Returns {pointer, size} for a Windows resource embedded with embed_binary_rc.
 * The resource name must match the resource_name attribute in the BUILD rule.
 */
inline std::pair<const uint8_t*, size_t> getEmbeddedWasmResource() {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, L"MOZJS_WASM_CWASM", RT_RCDATA);
    HGLOBAL handle = LoadResource(mod, res);
    const uint8_t* data = static_cast<const uint8_t*>(LockResource(handle));
    size_t size = SizeofResource(mod, res);
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
