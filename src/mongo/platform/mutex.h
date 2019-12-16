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
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/registry_list.h"

namespace mongo {

class Mutex;

namespace latch_detail {

using Level = hierarchical_acquisition_detail::Level;

static constexpr auto kAnonymousName = "AnonymousLatch"_sd;

/**
 * An Identity encapsulates the context around a latch
 */
class Identity {
public:
    Identity() : Identity(boost::none, kAnonymousName) {}

    explicit Identity(StringData name) : Identity(boost::none, name) {}

    Identity(boost::optional<Level> level, StringData name)
        : _index(_nextIndex()), _level(level), _name(name.toString()) {}

    /**
     * Since SouceLocations usually come from macros, this function is a setter that allows
     * a SourceLocation to be paired with __VA_ARGS__ construction.
     */
    Identity& setSourceLocation(const SourceLocationHolder& sourceLocation) {
        invariant(!_sourceLocation);
        _sourceLocation = sourceLocation;
        return *this;
    }

    /**
     * Return an optional that may contain the SourceLocation for this latch
     */
    const boost::optional<SourceLocationHolder>& sourceLocation() const {
        return _sourceLocation;
    }

    /**
     * Return an optional that may contain the HierarchicalAcquisitionLevel for this latch
     */
    const boost::optional<Level>& level() const {
        return _level;
    }

    /**
     * Return the name for this latch
     *
     * If there was no name provided on construction, this will be latch_detail::kAnonymousName.
     */
    StringData name() const {
        return _name;
    }

    /**
     * Return the index for this latch
     *
     * Latch indexes are assigned as Identity objects are created. Any given ordering is only valid
     * for a single process lifetime.
     */
    size_t index() const {
        return _index;
    }

private:
    static int64_t _nextIndex() {
        static auto nextLatchIndex = AtomicWord<int64_t>(0);
        return nextLatchIndex.fetchAndAdd(1);
    }

    int64_t _index;
    boost::optional<Level> _level;
    std::string _name;

    boost::optional<SourceLocationHolder> _sourceLocation;
};

/**
 * This class holds working data for a latchable resource
 *
 * All member data is either i) synchronized or ii) constant.
 */
class Data {
public:
    explicit Data(Identity identity) : _identity(std::move(identity)) {}

    auto& counts() {
        return _counts;
    }

    const auto& counts() const {
        return _counts;
    }

    const auto& identity() const {
        return _identity;
    }

private:
    const Identity _identity;

    struct Counts {
        AtomicWord<int> created{0};
        AtomicWord<int> destroyed{0};

        AtomicWord<int> contended{0};
        AtomicWord<int> acquired{0};
        AtomicWord<int> released{0};
    };

    Counts _counts;
};

/**
 * latch_details::Catalog holds a collection of Data objects for use with Mutexes
 *
 * All rules for LockFreeCollection apply:
 * - Synchronization is provided internally
 * - All entries are expected to exist for the lifetime of the Catalog
 */
class Catalog final : public WeakPtrRegistryList<Data> {
public:
    static auto& get() {
        static Catalog gCatalog;
        return gCatalog;
    }
};

/**
 * Simple registration object that construct with an Identity and provides access to a Data
 *
 * This object actually owns the Data object to make lifetime management simpler.
 */
class Registration {
public:
    explicit Registration(Identity identity)
        : _data(std::make_shared<Data>(std::move(identity))), _index{Catalog::get().add(_data)} {}

    const auto& data() {
        return _data;
    }

private:
    std::shared_ptr<Data> _data;
    size_t _index;
};

/**
 * Get a Data object (Identity, Counts) for a unique type Tag (which can be a noop lambda)
 *
 * When used with a macro (or converted to have a c++20-style <typename Tag = decltype([]{})>), this
 * function provides a unique Data object per invocation context. This function also sets the
 * Identity identity to contain sourceLocation. This is explicitly intended to work with
 * preprocessor macros that generate SourceLocation objects and unique Tags.
 */
template <typename Tag>
auto getOrMakeLatchData(Tag&&, Identity identity, const SourceLocationHolder& sourceLocation) {
    static auto reg = Registration(  //
        std::move(identity)          //
            .setSourceLocation(sourceLocation));
    return reg.data();
}

/**
 * Provide a very generic Data object for use with default-constructed Mutexes
 */
inline auto defaultData() {
    return getOrMakeLatchData([] {}, Identity(kAnonymousName), MONGO_SOURCE_LOCATION());
}
}  // namespace latch_detail

class Latch {
public:
    virtual ~Latch() = default;

    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual bool try_lock() = 0;

    virtual StringData getName() const {
        return latch_detail::kAnonymousName;
    }
};

/**
 * Mutex is a Lockable type that wraps a stdx::mutex
 *
 * This class is intended to be used wherever a stdx::mutex would previously be used. It provides
 * a generic event-listener interface for instrumenting around lock()/unlock()/try_lock().
 */
class Mutex : public Latch {
public:
    class LockListener;

    void lock() override;
    void unlock() override;
    bool try_lock() override;
    StringData getName() const override;

    Mutex() : Mutex(latch_detail::defaultData()) {}
    explicit Mutex(std::shared_ptr<latch_detail::Data> data);

    ~Mutex();

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
            RegistryList<LockListener*> list;
        };

        static State state;
        return state;
    }

    void _onContendedLock() noexcept;
    void _onQuickLock() noexcept;
    void _onSlowLock() noexcept;
    void _onUnlock() noexcept;

    const std::shared_ptr<latch_detail::Data> _data;

    stdx::mutex _mutex;  // NOLINT
    bool _isLocked = false;
};

/**
 * A set of actions to happen upon notable events on a Lockable-conceptualized type
 */
class Mutex::LockListener {
    friend class Mutex;

public:
    using Identity = latch_detail::Identity;

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
 * Construct and register a latch_detail::Data object exactly once per call site
 */
#define MONGO_GET_LATCH_DATA(...)              \
    ::mongo::latch_detail::getOrMakeLatchData( \
        [] {}, ::mongo::latch_detail::Identity(__VA_ARGS__), MONGO_SOURCE_LOCATION_NO_FUNC())

/**
 * Construct a mongo::Mutex using the result of MONGO_GET_LATCH_DATA with all arguments forwarded
 */
#define MONGO_MAKE_LATCH(...) ::mongo::Mutex(MONGO_GET_LATCH_DATA(__VA_ARGS__));
