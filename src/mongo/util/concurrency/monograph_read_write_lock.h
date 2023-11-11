#pragma once

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/modules/monograph/tx_service/include/spinlock.h"

namespace mongo {
class ThreadlocalLock {
public:
    MONGO_DISALLOW_COPYING(ThreadlocalLock);
    explicit ThreadlocalLock(txservice::SimpleSpinlock& lock) : _lock(lock) {
        _lock.Lock();
    }
    ~ThreadlocalLock() {
        _lock.Unlock();
    }
    txservice::SimpleSpinlock& _lock;
};

class SyncAllThreadsLock {
public:
    MONGO_DISALLOW_COPYING(SyncAllThreadsLock);
    explicit SyncAllThreadsLock(std::vector<txservice::SimpleSpinlock>& lockVector)
        : _lockVector(lockVector) {
        for (auto& lk : _lockVector) {
            lk.Lock();
        }
    }

    ~SyncAllThreadsLock() {
        for (auto& lk : _lockVector) {
            lk.Unlock();
        }
    }
    std::vector<txservice::SimpleSpinlock>& _lockVector;
};
}  // namespace mongo