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

#include "mongo/db/fle_compact_cleanup_mutex.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"

#include <map>
#include <mutex>

namespace mongo {

class FLECompactCleanupMutex {
public:
    std::mutex mutex;
    stdx::condition_variable cv;
    bool isLocked = false;
};

namespace {

class FLECompactCleanupMutexRegistry {
public:
    std::shared_ptr<FLECompactCleanupMutex> getOrCreateMutexEntry(
        const NamespaceString& ecocLockNss) {
        std::lock_guard<std::mutex> lk(_mapMutex);
        auto it = _mutexes.find(ecocLockNss);
        if (it != _mutexes.end()) {
            if (auto state = it->second.lock()) {
                return state;
            }
        }

        auto state = std::make_shared<FLECompactCleanupMutex>();
        _mutexes[ecocLockNss] = state;
        return state;
    }

    void eraseMutexEntryIfUnused(const NamespaceString& ecocLockNss,
                                 const std::shared_ptr<FLECompactCleanupMutex>& state) {
        std::lock_guard<std::mutex> lk(_mapMutex);
        auto it = _mutexes.find(ecocLockNss);
        if (it == _mutexes.end()) {
            return;
        }

        auto current = it->second.lock();
        if (!current) {
            _mutexes.erase(it);
            return;
        }

        // 'current' is a temporary strong reference from weak_ptr::lock(), so a use count of 2
        // means the scoped object being destroyed is the only real owner.
        if (current == state && state.use_count() == 2) {
            _mutexes.erase(it);
        }
    }

    size_t sizeForTest() const {
        std::lock_guard<std::mutex> lk(_mapMutex);
        return _mutexes.size();
    }

private:
    mutable std::mutex _mapMutex;
    std::map<NamespaceString, std::weak_ptr<FLECompactCleanupMutex>> _mutexes;
};

const auto getFLECompactCleanupMutexRegistry =
    ServiceContext::declareDecoration<FLECompactCleanupMutexRegistry>();

}  // namespace

ScopedFLECompactCleanupMutex::ScopedFLECompactCleanupMutex(OperationContext* opCtx,
                                                           const NamespaceString& ecocLockNss)
    : _opCtx(opCtx),
      _ecocLockNss(ecocLockNss),
      _mutex(getFLECompactCleanupMutexRegistry(opCtx->getServiceContext())
                 .getOrCreateMutexEntry(ecocLockNss)) {
    try {
        std::unique_lock<std::mutex> lk(_mutex->mutex);
        opCtx->waitForConditionOrInterrupt(_mutex->cv, lk, [this] { return !_mutex->isLocked; });
        _mutex->isLocked = true;
    } catch (...) {
        getFLECompactCleanupMutexRegistry(_opCtx->getServiceContext())
            .eraseMutexEntryIfUnused(_ecocLockNss, _mutex);
        throw;
    }
}

ScopedFLECompactCleanupMutex::~ScopedFLECompactCleanupMutex() {
    {
        std::lock_guard<std::mutex> lk(_mutex->mutex);
        invariant(_mutex->isLocked);
        _mutex->isLocked = false;
    }
    _mutex->cv.notify_one();

    getFLECompactCleanupMutexRegistry(_opCtx->getServiceContext())
        .eraseMutexEntryIfUnused(_ecocLockNss, _mutex);
}

size_t getFLECompactCleanupMutexRegistrySizeForTest(ServiceContext* serviceContext) {
    return getFLECompactCleanupMutexRegistry(serviceContext).sizeForTest();
}

}  // namespace mongo
