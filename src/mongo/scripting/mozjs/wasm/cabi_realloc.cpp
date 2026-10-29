// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
