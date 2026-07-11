// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <cstddef>
#include <cstdint>

namespace mongo {
namespace mozjs {
namespace wasm {
typedef struct {
    size_t opTimeout;           // microseconds (0 = no timeout)
    size_t heapSize;            // in MB
    bool javascriptProtection;  // mirrors --enableJavaScriptProtection
} wasm_mozjs_startup_options_t;

}  // namespace wasm
}  // namespace mozjs
}  // namespace mongo

