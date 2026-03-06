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

#include <cstdio>
#include <cstring>

extern "C" void* cabi_realloc(void* ptr, size_t old_size, size_t align, size_t new_size);

namespace mongo {
namespace mozjs {
namespace wasm {

/**
 * Set a string field, allocating memory via cabi_realloc.
 * If the field already has a value, it will be freed first.
 */
inline void set_string(char** dst, size_t* dst_len, const char* src) {
    if (!dst || !dst_len)
        return;

    // Free existing string
    if (*dst) {
        cabi_realloc(*dst, *dst_len + 1, 1, 0);
        *dst = nullptr;
        *dst_len = 0;
    }

    if (!src || src[0] == '\0') {
        return;
    }

    size_t src_len = std::strlen(src);

    char* allocated = static_cast<char*>(cabi_realloc(nullptr, 0, 1, src_len + 1));
    if (!allocated) {
        return;
    }

    std::memcpy(allocated, src, src_len);
    allocated[src_len] = '\0';

    *dst = allocated;
    *dst_len = src_len;
}

/**
 * Copy a C string into a fixed-size output buffer (always null-terminates).
 */
inline void set_cstr(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, cap, "%s", src);
}

}  // namespace wasm
}  // namespace mozjs
}  // namespace mongo
