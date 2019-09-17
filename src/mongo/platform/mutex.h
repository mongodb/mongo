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
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"

namespace mongo {

class Latch {
public:
    virtual ~Latch() = default;

    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual bool try_lock() = 0;
};

class Mutex : public Latch {
public:
    class LockActions;
    static constexpr auto kAnonymousMutexStr = "AnonymousMutex"_sd;

    Mutex() : Mutex(kAnonymousMutexStr) {}
    // Note that StringData is a view type, thus the underlying string for _name must outlive any
    // given Mutex
    explicit Mutex(const StringData& name) : _name(name) {}

    void lock() override;
    void unlock() override;
    bool try_lock() override;
    const StringData& getName() const {
        return _name;
    }

private:
    const StringData _name;
    stdx::mutex _mutex;  // NOLINT
};

/**
 * A set of actions to happen upon notable events on a Lockable-conceptualized type
 */
class Mutex::LockActions {
    friend class Mutex;

public:
    virtual ~LockActions() = default;
    /**
     * Action to do when a lock cannot be immediately acquired
     */
    virtual void onContendedLock(const StringData& name) = 0;

    /**
     * Action to do when a lock is unlocked
     */
    virtual void onUnlock(const StringData& name) = 0;

    /**
     * This function adds a LockActions subclass to the triggers for certain actions.
     *
     * Note that currently there is only one LockActions in use at a time. As part of SERVER-42895,
     * this will change so that there is a list of LockActions maintained.
     *
     * LockActions can only be added and not removed. If you wish to deactivate a LockActions
     * subclass, please provide the switch on that subclass to noop its functions.
     */
    static void add(LockActions* actions);

private:
    static auto& getState() {
        struct State {
            AtomicWord<LockActions*> actions{nullptr};
        };
        static State state;
        return state;
    }
};

}  // namespace mongo

/**
 * Define a mongo::Mutex with all arguments passed through to the ctor
 */
#define MONGO_MAKE_LATCH(...) \
    mongo::Mutex {            \
        __VA_ARGS__           \
    }
