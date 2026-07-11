// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/thread_safety_context.h"

#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"

namespace mongo {

ThreadSafetyContext* ThreadSafetyContext::getThreadSafetyContext() noexcept {
    static auto safetyContext = new ThreadSafetyContext();  // Intentionally leaked
    return safetyContext;
}

void ThreadSafetyContext::forbidMultiThreading() noexcept {
    invariant(_isSingleThreaded.load());
    invariant(_safeToCreateThreads.swap(false));
}

void ThreadSafetyContext::allowMultiThreading() noexcept {
    invariant(_isSingleThreaded.load());
    invariant(!_safeToCreateThreads.swap(true));
}

void ThreadSafetyContext::onThreadCreate() noexcept {
    invariant(_safeToCreateThreads.load());
    if (MONGO_unlikely(_isSingleThreaded.load())) {
        _isSingleThreaded.store(false);
    }
}

}  // namespace mongo
