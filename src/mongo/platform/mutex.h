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
#include "mongo/platform/source_location.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {

class Latch {
public:
    virtual ~Latch() = default;

    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual bool try_lock() = 0;

    virtual StringData getName() const {
        return "AnonymousLatch"_sd;
    }
};

class Mutex : public Latch {
    class LockNotifier;

public:
    class LockListener;

    static constexpr auto kAnonymousMutexStr = "AnonymousMutex"_sd;

    void lock() override;
    void unlock() override;
    bool try_lock() override;
    StringData getName() const override {
        return StringData(_id.name);
    }

    struct Identity {
        Identity(StringData name = kAnonymousMutexStr) : Identity(boost::none, boost::none, name) {}

        Identity(SourceLocationHolder sourceLocation, StringData name = kAnonymousMutexStr)
            : Identity(boost::none, sourceLocation, name) {}

        Identity(hierarchical_acquisition_detail::Level level, StringData name = kAnonymousMutexStr)
            : Identity(level, boost::none, name) {}

        Identity(boost::optional<hierarchical_acquisition_detail::Level> level,
                 boost::optional<SourceLocationHolder> sourceLocation,
                 StringData name = kAnonymousMutexStr)
            : level(level), sourceLocation(sourceLocation), name(name.toString()) {}

        boost::optional<hierarchical_acquisition_detail::Level> level;
        boost::optional<SourceLocationHolder> sourceLocation;
        std::string name;
    };

    Mutex() : Mutex(Identity()) {}

    Mutex(const Identity& id) : _id(id) {}

    struct LatchSetState {
        hierarchical_acquisition_detail::Set levelsHeld;
    };

    /**
     * This function adds a LockListener subclass to the triggers for certain actions.
     *
     * LockListeners can only be added and not removed. If you wish to deactivate a LockListeners
     * subclass, please provide the switch on that subclass to noop its functions. It is only safe
     * to add a LockListener during a MONGO_INITIALIZER.
     */
    static void addLockListener(LockListener* listener);

private:
    static auto& _getListenerState() noexcept {
        struct State {
            std::vector<LockListener*> list;
        };

        // Note that state should no longer be mutated after init-time (ala MONGO_INITIALIZERS). If
        // this changes, than this state needs to be synchronized.
        static State state;
        return state;
    }

    static void _onContendedLock(const Identity& id) noexcept;
    static void _onQuickLock(const Identity& id) noexcept;
    static void _onSlowLock(const Identity& id) noexcept;
    static void _onUnlock(const Identity& id) noexcept;

    const Identity _id;

    stdx::mutex _mutex;  // NOLINT
};

/**
 * A set of actions to happen upon notable events on a Lockable-conceptualized type
 */
class Mutex::LockListener {
    friend class Mutex;

public:
    virtual ~LockListener() = default;

    /**
     * Action to do when a lock cannot be immediately acquired
     */
    virtual void onContendedLock(const Identity& id) = 0;

    /**
     * Action to do when a lock was acquired without blocking
     */
    virtual void onQuickLock(const Identity& id) = 0;

    /**
     * Action to do when a lock was acquired after blocking
     */
    virtual void onSlowLock(const Identity& id) = 0;

    /**
     * Action to do when a lock is unlocked
     */
    virtual void onUnlock(const Identity& id) = 0;
};

}  // namespace mongo

/**
 * Define a mongo::Mutex with all arguments passed through to the ctor
 */
#define MONGO_MAKE_LATCH(...)               \
    mongo::Mutex {                          \
        mongo::Mutex::Identity(__VA_ARGS__) \
    }
