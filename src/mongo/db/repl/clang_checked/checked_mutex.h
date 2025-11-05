/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/clang_checked/lockable_concepts.h"
#include "mongo/db/repl/clang_checked/thread_safety_annotations.h"
#include "mongo/util/modules.h"

#include <concepts>

namespace MONGO_MOD_PUB mongo {
namespace clang_checked {

/**
 * CheckedMutex is a wrapper for other mutex types for cases where clang's
 * thread safety checks are desirable. By combining CheckedMutex and the
 * annotations defined in thread_safety_annotations.h in this directory, you
 * can enforce constraints such as "this member is always guarded by that
 * mutex."
 */
template <BaseLockable MutexT>
class MONGO_LOCKING_CAPABILITY("mutex") CheckedMutex {
public:
    using mutex_type = MutexT;

    void lock() MONGO_LOCKING_ACQUIRE() {
        _m.lock();
    }

    bool try_lock()
    requires TryLockable<mutex_type>
    MONGO_LOCKING_TRY_ACQUIRE(true) {
        return _m.try_lock();
    }

    void unlock() MONGO_LOCKING_RELEASE() {
        _m.unlock();
    }

private:
    mutable mutex_type _m;
};

}  // namespace clang_checked
}  // namespace MONGO_MOD_PUB mongo
