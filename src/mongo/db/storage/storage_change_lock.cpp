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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/storage_change_lock.h"

#include "mongo/util/time_support.h"

namespace mongo {
/**
 * SharedSpinLock routines.
 *
 * The spin lock's lock word is logically divided into a bit (kExclusiveLock) and an unsigned
 * integer value for the rest of the word.  The meanings are as follows:
 *
 * kExclusiveLock not set, rest of word 0: Lock not held nor waited on.
 *
 * kExclusiveLock not set, rest of word non-zero: Lock held in shared mode by the number of holders
 * specified in the rest of the word.
 *
 * kExclusiveLock set, rest of word 0: Lock held in exclusive mode, no shared waiters.
 *
 * kExclusiveLock set, rest of word non-zero: Lock held in exclusive mode, number of waiters for the
 * shared lock specified in the rest of the word.
 *
 * Note that if there are shared waiters when the exclusive lock is released, they will obtain the
 * lock before another exclusive lock can be obtained.  This should be considered an implementation
 * detail and not a guarantee.
 *
 */
void StorageChangeLock::SharedSpinLock::lock() {
    uint32_t expected = 0;
    while (!_lockWord.compareAndSwap(&expected, kExclusiveLock)) {
        expected = 0;
        mongo::sleepmillis(100);
    }
}

void StorageChangeLock::SharedSpinLock::unlock() {
    uint32_t prevLockWord = _lockWord.fetchAndBitAnd(~kExclusiveLock);
    invariant(prevLockWord & kExclusiveLock);
}

void StorageChangeLock::SharedSpinLock::lock_shared() {
    uint32_t prevLockWord = _lockWord.fetchAndAdd(1);
    // If the shared part of the lock word was all-ones, we just overflowed it.  This requires
    // 2^31 threads creating an opCtx at once, which shouldn't happen.
    invariant((prevLockWord & ~kExclusiveLock) != ~kExclusiveLock);
    while (MONGO_unlikely(prevLockWord & kExclusiveLock)) {
        mongo::sleepmillis(kLockPollIntervalMillis);
        prevLockWord = _lockWord.load();
    }
}

void StorageChangeLock::SharedSpinLock::unlock_shared() {
    uint32_t prevLockWord = _lockWord.fetchAndSubtract(1);
    invariant(!(prevLockWord & kExclusiveLock));
}
}  // namespace mongo
