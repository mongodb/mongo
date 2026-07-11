// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
