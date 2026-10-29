// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/platform/stack_locator.h"
#include "mongo/util/assert_util.h"

#include <pthread.h>

namespace mongo {

StackLocator::StackLocator(const void* capturedStackPointer)
    : _capturedStackPointer(capturedStackPointer) {
    const auto self = pthread_self();
    _begin = pthread_get_stackaddr_np(self);
    invariant(_begin);

    const auto size = pthread_get_stacksize_np(self);
    invariant(size);

    // TODO: Assumes stack grows downward on OS X.
    _end = static_cast<const char*>(_begin) - size;
}

}  // namespace mongo
