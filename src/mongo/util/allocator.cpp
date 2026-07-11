// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/signal_handlers_synchronous.h"

#include <cstdlib>

namespace mongo {

void* mongoMalloc(size_t size) {
    void* x = std::malloc(size);
    if (x == nullptr) {
        reportOutOfMemoryErrorAndExit();
    }
    return x;
}

void* mongoRealloc(void* ptr, size_t size) {
    void* x = std::realloc(ptr, size);
    if (x == nullptr) {
        reportOutOfMemoryErrorAndExit();
    }
    return x;
}

}  // namespace mongo
