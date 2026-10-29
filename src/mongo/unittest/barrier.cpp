// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// IWYU pragma: no_include "cxxabi.h"


#include "mongo/unittest/barrier.h"

#include "mongo/util/assert_util.h"

namespace mongo {
namespace unittest {

Barrier::Barrier(size_t threadCount)
    : _threadCount(threadCount), _threadsWaiting(threadCount), _generation(0) {
    invariant(_threadCount > 0);
}

void Barrier::countDownAndWait() {
    std::unique_lock<std::mutex> _lock(_mutex);
    _threadsWaiting--;
    if (_threadsWaiting == 0) {
        _generation++;
        _threadsWaiting = _threadCount;
        _condition.notify_all();
    } else {
        uint64_t currentGeneration = _generation;
        while (currentGeneration == _generation) {
            _condition.wait(_lock);
        }
    }
}

}  // namespace unittest
}  // namespace mongo
