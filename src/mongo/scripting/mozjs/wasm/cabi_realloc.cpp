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

#include <cstdlib>

// mozglue stub for rust hooks
extern "C" void install_rust_hooks() {}

// Canonical ABI allocator hook used by the component model.
// Required for wasm-component-ld to produce a valid component.
extern "C" void* cabi_realloc(void* ptr, size_t old_size, size_t align, size_t new_size) {
    (void)old_size;
    (void)align;
    if (new_size == 0) {
        std::free(ptr);
        return nullptr;
    }
    void* result = std::realloc(ptr, new_size);
    if (!result) {
        // realloc failed -- original ptr is still valid but we cannot grow.
        // In WASM component model this is typically fatal.
        std::abort();
    }
    return result;
}
