/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include <shared_mutex>

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
class StorageChangeLock {
    /**
     * Token to be held by caller while changing the storage engine on the context.
     */
private:
    class SharedSpinLock;

public:
    using Token = stdx::unique_lock<SharedSpinLock>;

    /**
     * Acquires the storage change lock in shared mode and returns an RAII lock object to it.
     */
    auto acquireSharedStorageChangeToken() {
        // TODO(SERVER-59157): Replace use of std::shared_lock with stdx::shared_lock or remove
        // NOLINT according to resolution of this ticket.
        return std::shared_lock(_storageChangeSpinlock);  // NOLINT
    }

    Token acquireExclusiveStorageChangeToken() {
        return stdx::unique_lock(_storageChangeSpinlock);  // NOLINT
    }

private:
    // Spin lock for storage change.  Needs to be fast for lock_shared and unlock_shared,
    // not for the exclusive lock.  This lock has no fairness guarantees and is not re-entrant
    // from shared -> exclusive (i.e. it cannot be upgraded), exclusive -> shared,
    // or exclusive -> exclusive.
    class SharedSpinLock {
    public:
        void lock();
        void unlock();
        void lock_shared();
        void unlock_shared();

    private:
        AtomicWord<uint32_t> _lockWord;
        static const uint32_t kExclusiveLock = 1 << 31;
        static const int kLockPollIntervalMillis = 100;
    };

    SharedSpinLock _storageChangeSpinlock;
};
}  // namespace mongo
