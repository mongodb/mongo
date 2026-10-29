// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/platform/stack_locator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <pthread.h>

namespace mongo {

StackLocator::StackLocator(const void* capturedStackPointer)
    : _capturedStackPointer(capturedStackPointer) {
    pthread_t self = pthread_self();
    pthread_attr_t selfAttrs;
    invariant(pthread_attr_init(&selfAttrs) == 0);
    invariant(pthread_getattr_np(self, &selfAttrs) == 0);
    ON_BLOCK_EXIT([&] { pthread_attr_destroy(&selfAttrs); });

    void* base = nullptr;
    size_t size = 0;

    auto result = pthread_attr_getstack(&selfAttrs, &base, &size);

    invariant(result == 0);
    invariant(base != nullptr);
    invariant(size != 0);

    // TODO: Assumes a downward growing stack. Note here that
    // getstack returns the stack *base*, being the bottom of the
    // stack, so we need to add size to it.
    _end = base;
    _begin = static_cast<const char*>(_end) + size;
}

}  // namespace mongo
