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

#include <cstdint>

#include "mongo/platform/atomic_word.h"

namespace mongo {
class StorageChangeLock {
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
        static constexpr uint32_t kExclusiveLock = 1 << 31;
        static constexpr int kLockPollIntervalMillis = 100;
    };

public:
    void lock() {
        _storageChangeSpinlock.lock();
    }

    void unlock() {
        _storageChangeSpinlock.unlock();
    }

    void lock_shared() {
        _storageChangeSpinlock.lock_shared();
    }

    void unlock_shared() {
        _storageChangeSpinlock.unlock_shared();
    }

private:
    SharedSpinLock _storageChangeSpinlock;
};
}  // namespace mongo
