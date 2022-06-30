/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"


namespace mongo {
/**
 * RAII object that unlocks a unique_lock type on construction, and relocks it on destruction. The
 * unique_lock must be locked when it is given to ScopedUnlock.
 */
template <typename T>
class ScopedUnlock {
public:
    /**
     * Construct a new Scoped Unlock object.
     * Unique_locks passed into this constructor must be locked, or an invariant failure will be
     * thrown.
     */
    explicit ScopedUnlock(stdx::unique_lock<T>& lock) : _lock(lock) {
        invariant(_lock.owns_lock(), "Locks in ScopedUnlock must be locked on initialization.");
        _lock.unlock();
    }

    ~ScopedUnlock() {
        if (!_dismissed) {
            _lock.lock();
        }
    }

    ScopedUnlock(const ScopedUnlock&) = delete;
    ScopedUnlock(ScopedUnlock&&) = delete;
    ScopedUnlock& operator=(const ScopedUnlock&) = delete;
    ScopedUnlock& operator=(ScopedUnlock&&) = delete;

    /** A dismissed ScopedUnlock does not lock on destruction. */
    void dismiss() noexcept {
        _dismissed = true;
    }

private:
    stdx::unique_lock<T>& _lock;
    bool _dismissed = false;
};

}  // namespace mongo
