// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
