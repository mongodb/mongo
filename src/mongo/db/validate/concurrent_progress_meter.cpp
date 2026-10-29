// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/concurrent_progress_meter.h"

#include <mutex>

namespace mongo {

void ConcurrentProgressMeterHolder::set(WithLock,
                                        ProgressMeter& pm,
                                        OperationContext* opCtx,
                                        int64_t flushThreshold) {
    _pm = &pm;
    _parentOpCtx = opCtx;
    _flushThreshold = flushThreshold;
    _pendingHits.store(0);
}

void ConcurrentProgressMeterHolder::hit(int64_t n) {
    if (_pendingHits.addAndFetch(n) >= _flushThreshold) {
        _flush();
    }
}

void ConcurrentProgressMeterHolder::finished() {
    const int hits = _pendingHits.swap(0);
    std::unique_lock<Client> lk(*_parentOpCtx->getClient());
    if (hits > 0) {
        _pm->hit(hits);
    }
    _pm->finished();
}

void ConcurrentProgressMeterHolder::_flush() {
    const auto hits = _pendingHits.swap(0);
    if (hits > 0) {
        std::unique_lock<Client> lk(*_parentOpCtx->getClient());
        _pm->hit(hits);
    }
}

}  // namespace mongo
