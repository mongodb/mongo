/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <type_traits>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

class LockActions {
public:
    virtual ~LockActions() = default;
    virtual void onContendedLock(const StringData& name) = 0;
    virtual void onUnlock() = 0;
    virtual void onFailedLock() = 0;
};

class Mutex {
public:
    static constexpr auto kAnonymousMutexStr = "AnonymousMutex"_sd;

    Mutex() : Mutex(kAnonymousMutexStr) {}
    // Note that StringData is a view type, thus the underlying string for _name must outlive any
    // given Mutex
    explicit Mutex(const StringData& name) : _name(name) {}
    explicit Mutex(const StringData& name, Seconds lockTimeout)
        : _name(name), _lockTimeout(lockTimeout) {}

    void lock();
    void unlock();
    bool try_lock();
    const StringData& getName() const {
        return _name;
    }

    static void setLockActions(std::unique_ptr<LockActions> actions);

private:
    const StringData _name;
    const Seconds _lockTimeout = Seconds(60);
    static constexpr Milliseconds kContendedLockTimeout = Milliseconds(100);
    stdx::timed_mutex _mutex;
};

}  // namespace mongo

/**
 * Define a mongo::Mutex with all arguments passed through to the ctor
 */
#define MONGO_MAKE_LATCH(...) \
    mongo::Mutex {            \
        __VA_ARGS__           \
    }
