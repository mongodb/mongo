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

#include <functional>
#include <string>
#include <type_traits>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/cancelation.h"
#include "mongo/util/duration.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 *
 * A FailPoint is a hook mechanism allowing testing behavior to occur at prearranged
 * execution points in the server code. They can be activated and deactivated, and
 * configured to hold data.
 *
 * A FailPoint is usually defined by the MONGO_FAIL_POINT_DEFINE(name) macro,
 * which arranges for it to be added to the global failpoint registry.
 *
 * A FailPoint object can have unusual lifetime semantics. It can be marked
 * `immortal`, so that its internal state is never destroyed. This is possible
 * because FailPoint is designed to have only trivially destructible nonstatic
 * data members, and we can choose not to manually destroy the internal state
 * object. This enables server code that is instrumented by an immortal
 * static-duration FailPoint to remain valid even during process shutdown.
 *
 * Sample use:
 *
 * // Defined somewhere:
 * MONGO_FAIL_POINT_DEFINE(failPoint);
 *
 * bool somewhereInTheCode() {
 *   ... do some stuff ...
 *   // The failpoint artificially changes the return value of this function when active.
 *   if (MONGO_unlikely(failPoint.shouldFail()))
 *       return false;
 *   return true;
 * }
 *
 * - or to implement more complex scenarios, use execute/executeIf -
 *
 * bool somewhereInTheCode() {
 *     failPoint.execute([&](const BSONObj& data) {
 *         // The bad things happen here, and can read the injected 'data'.
 *     });
 *     return true;
 * }
 *
 * // scoped() is another way to do it, where lambda isn't suitable, e.g. to cause
 * // a return/continue/break to control the enclosing function.
 * for (auto& user : users) {
 *     // The failpoint can be activated and given a user name, to skip that user.
 *     if (auto sfp = failPoint.scoped(); MONGO_unlikely(sfp.isActive())) {
 *         if (sfp.getData()["user"] == user.name()) {
 *             continue;
 *         }
 *     }
 *     processOneUser(user);
 * }
 *
 * // Rendered compactly with scopedIf where the data serves as an activation filter.
 * for (auto& user : users) {
 *     if (MONGO_unlikely(failPoint.scopedIf([&](auto&& o) {
 *         return o["user"] == user.name();
 *     }).isActive())) {
 *         continue;
 *     }
 *     processOneUser(user);
 * }
 *
 *  The `scopedIf` and `executeIf` members have an advantage over `scoped` and `execute`. They
 *  only affect the `FailPoint` activation counters (relevant to the `nTimes` and `skip` modes)
 *  if the predicate is true.
 *
 * A FailPoint can be configured remotely by a database command.
 * See `src/mongo/db/commands/fail_point_cmd.cpp`.
 *
 */
class FailPoint {
public:
    using ValType = unsigned;
    enum Mode { off, alwaysOn, random, nTimes, skip };

    struct ModeOptions {
        Mode mode;
        ValType val;
        BSONObj extra;
    };

    // long long values are able to be appended to BSON. If this is using declaration is changed,
    // please make sure that the new type is also BSON-compatible.
    using EntryCountT = long long;

private:
    class Impl {
    private:
        enum class AlreadyCounted : bool {};

        static constexpr auto _kWaitGranularity = Milliseconds(100);

        static constexpr auto _kActiveBit = ValType{ValType{1} << 31};

    public:
        class LockHandle {
        public:
            LockHandle(Impl* impl, bool hit) : _impl(impl), _hit(hit) {}

            ~LockHandle() {
                if (MONGO_unlikely(_impl))
                    _impl->_unlock();
            }

            LockHandle(const LockHandle&) = delete;
            LockHandle& operator=(const LockHandle&) = delete;
            LockHandle(LockHandle&& o) noexcept
                : _impl{std::exchange(o._impl, nullptr)}, _hit{std::exchange(o._hit, false)} {}
            LockHandle& operator=(LockHandle&&) = delete;

            /**
             * Returns true if this LockHandle associated with a FailPoint, and
             * the lock outcome was a "hit". `lockHandle.isActive()` generally
             * means the block of FailPoint special behavior should execute.
             */
            bool isActive() const {
                return MONGO_unlikely(_hit);
            }

            /** May only be called if isActive() is true. */
            const BSONObj& getData() const {
                invariant(_impl, "getData without holding failpoint lock");
                return _impl->_data;
            }

        private:
            Impl* _impl = nullptr;
            bool _hit = false;  //< True if this represents a tryLock "hit".
        };

        Impl(std::string name) : _name(std::move(name)) {}

        template <typename Pred>
        bool shouldFail(Pred&& pred) {
            return _shouldFail(AlreadyCounted{false}, pred);
        }

        EntryCountT setMode(Mode mode, ValType val = 0, BSONObj extra = {});

        EntryCountT waitForTimesEntered(Interruptible* interruptible,
                                        EntryCountT targetTimesEntered) const;

        BSONObj toBSON() const;

        template <typename Pred>
        LockHandle tryLock(Pred&& pred) {
            return _tryLock(AlreadyCounted{false}, pred);
        }

        /** See `FailPoint::pauseWhileSet`. */
        void pauseWhileSet(Interruptible* interruptible) {
            auto alreadyCounted = AlreadyCounted{false};
            while (MONGO_unlikely(_shouldFail(alreadyCounted, nullptr))) {
                interruptible->sleepFor(_kWaitGranularity);
                alreadyCounted = AlreadyCounted{true};
            }
        }

        /** See `FailPoint::pauseWhileSetAndNotCanceled`. */
        void pauseWhileSetAndNotCanceled(Interruptible* interruptible,
                                         const CancelationToken& token) {
            auto alreadyCounted = AlreadyCounted{false};
            while (MONGO_unlikely(_shouldFail(alreadyCounted, nullptr)) && !token.isCanceled()) {
                interruptible->sleepFor(_kWaitGranularity);
                alreadyCounted = AlreadyCounted{true};
            }
        }

        const std::string& getName() const {
            return _name;
        }

    private:
        void _enable() {
            _fpInfo.fetchAndBitOr(_kActiveBit);
        }

        void _disable() {
            _fpInfo.fetchAndBitAnd(~_kActiveBit);
        }

        /** No default parameters. No-Frills shouldFail implementation. */
        template <typename Pred>
        bool _shouldFail(AlreadyCounted alreadyCounted, Pred&& pred) {
            return _tryLock(alreadyCounted, pred).isActive();
        }

        /**
         * Release a FailPoint lock previously acquired with `_tryLock`.
         * Used only by `~LockHandle`.
         */
        void _unlock() {
            _fpInfo.subtractAndFetch(1);
        }

        /**
         * Attempt to access the fail point. If FailPoint is disabled, it
         * cannot be accessed and this call will return a disengaged and
         * inactive LockHandle.
         *
         * After successfully locking it, however, the caller will have
         * received either a hit or a miss, observable by calling
         * `result.isActive()`.  If true, then caller may further access the
         * associated `const BSONObj&` payload with `result.getData()`.
         *
         * If `pred` is callable, `pred(data)` is invoked with the FailPoint
         * BSON data payload. If it returns false, it specifies a user-defined
         * Failpoint miss. In response, this function will return an inactive
         * LockHandle.
         *
         * Otherwise the FailPoint determines whether this lock operation
         * outcome is a hit or a miss based on the FailPoint's configured Mode.
         *
         * Unless `alreadyCounted` is true, such a hit will also increment
         * `_hitCount` as a side effect. This complication enables the
         * `pauseWhileSet` loop to evaluate the failpoint multiple times while
         * only counting the first of those hits in terms of the `_hitCount`.
         */
        template <typename Pred>
        LockHandle _tryLock(AlreadyCounted alreadyCounted, Pred&& pred) {
            if (MONGO_likely((_fpInfo.loadRelaxed() & _kActiveBit) == 0))
                return LockHandle{nullptr, false};  // Fast path

            if ((_fpInfo.addAndFetch(1) & _kActiveBit) == 0)
                return LockHandle{this, false};  // Took a reference to disabled in data race.

            // Slow path. Wrap in `std::function` to deal with nullptr_t
            // or other predicates that are not bool-convertible.
            std::function<bool(const BSONObj&)> predWrap(std::move(pred));

            // The caller-supplied predicate, if provided, can force a miss that
            // bypasses the `_evaluateByMode()` call.
            bool bypass = predWrap && !predWrap(_data);
            bool hit = bypass ? false : _evaluateByMode();

            if (hit && alreadyCounted == AlreadyCounted{false})
                _hitCount.addAndFetch(1);
            return LockHandle{this, hit};
        }

        /**
         * Use the configured mode to determine hit or miss.
         * Return true to indicate a hit
         */
        bool _evaluateByMode();

        // Bit layout:
        // 31: tells whether this fail point is active.
        // 0~30: ref counter: # of outstanding LockHandles.
        AtomicWord<std::uint32_t> _fpInfo{0};

        /* Number of times this has been locked with a `hit` result. */
        AtomicWord<EntryCountT> _hitCount{0};

        // Invariant: These should be read only if _kActiveBit of _fpInfo is set.
        Mode _mode{off};
        AtomicWord<int> _modeValue{0};
        BSONObj _data;

        const std::string _name;

        // protects _mode, _modeValue, _data
        mutable Mutex _modMutex = MONGO_MAKE_LATCH("FailPoint::_modMutex");
    };

public:
    /**
     * An object representing a FailPoint's interaction with the code it is
     * instrumenting. Users don't create these. They are only used within the
     * execute and executeIf functions and returned by the scoped() and
     * scopedIf() functions.
     *
     * If the FailPoint access attempt does not acquire a reference to the
     * FailPoint, the returned LockHandle will be disengaged. Otherwise, it
     * holds a reference to its associated FailPoint, ensuring that FailPoint's
     * state doesn't change while a LockHandle is attached to it.
     *
     * Even an engaged LockHandle (holds a reference to a FailPoint)
     * can still have `isActive()==false`.
     *
     * LockHandle `isActive()`, then `getData()` may be called on it to
     * retrieve injected data from the associated FailPoint.
     *
     * Ex:
     *     if (auto scoped = failPoint.scoped(); scoped.isActive()) {
     *         const BSONObj& data = scoped.getData();
     *         //  failPoint injects some behavior, informed by `data`.
     *     }
     */
    using LockHandle = Impl::LockHandle;

    /**
     * Explicitly resets the seed used for the PRNG in this thread.  If not called on a thread,
     * an instance of SecureRandom is used to seed the PRNG.
     */
    static void setThreadPRNGSeed(int32_t seed);

    /**
     * Parses the {mode, val, extra} from the BSON.
     * obj = {
     *   mode: modeElem // required
     *   data: extra    // optional payload to inject into the FailPoint intercept site.
     * }
     * where `modeElem` is one of:
     *       "off"
     *       "alwaysOn"
     *       {"times" : val}   // active for the next val calls
     *       {"skip" : val}    // skip calls, activate on and after call number (val+1).
     *       {"activationProbability" : val}  // val is in interval [0.0, 1.0]
     */
    static StatusWith<ModeOptions> parseBSON(const BSONObj& obj);

    /**
     * FailPoint state can be kept alive during shutdown by setting `immortal` true.
     * The usual macro definition does this, but FailPoint unit tests do not.
     */
    explicit FailPoint(std::string name, bool immortal = false);

    FailPoint(const FailPoint&) = delete;
    FailPoint& operator=(const FailPoint&) = delete;

    /**
     * If this FailPoint was constructed as `immortal` (FailPoints defined by
     * MONGO_FAIL_POINT_DEFINE are immortal), this destructor does nothing. In
     * that case the FailPoint (and the code it is instrumenting) can operate
     * normally while the process shuts down.
     */
    ~FailPoint();

    const std::string& getName() const {
        return _impl()->getName();
    }

    /**
     * Returns true if fail point is active.
     *
     * @param pred       see `executeIf` for more information.
     *
     * Calls to `shouldFail` should be placed inside MONGO_unlikely for performance.
     *    if (MONGO_unlikely(failpoint.shouldFail())) ...
     */
    template <typename Pred>
    bool shouldFail(Pred&& pred) {
        return _impl()->shouldFail(pred);
    }

    bool shouldFail() {
        return shouldFail(nullptr);
    }

    /**
     * Changes the settings of this fail point. This will turn off the FailPoint and
     * wait for all references on this FailPoint to go away before modifying it.
     *
     * @param mode  new mode
     * @param val   unsigned having different interpretations depending on the mode:
     *
     *     - off, alwaysOn: ignored
     *     - random: static_cast<int32_t>(std::numeric_limits<int32_t>::max() * p), where
     *           where p is the probability that any given evaluation of the failpoint should
     *           activate.
     *     - nTimes: the number of times this fail point will be active when
     *         #shouldFail/#execute/#scoped are called.
     *     - skip: will become active and remain active after
     *         #shouldFail/#execute/#scoped are called this number of times.
     *
     * @param extra arbitrary BSON object that can be stored to this fail point
     *     that can be referenced afterwards with #getData. Defaults to an empty
     *     document.
     *
     * @returns the number of times the fail point has been entered so far.
     */
    EntryCountT setMode(Mode mode, ValType val = 0, BSONObj extra = {}) {
        return _impl()->setMode(std::move(mode), std::move(val), std::move(extra));
    }

    EntryCountT setMode(ModeOptions opt) {
        return setMode(std::move(opt.mode), std::move(opt.val), std::move(opt.extra));
    }

    /**
     * Waits until the fail point has been entered the desired number of times.
     *
     * @param targetTimesEntered the number of times the fail point has been entered.
     *
     * @returns the number of times the fail point has been entered so far.
     */
    EntryCountT waitForTimesEntered(EntryCountT targetTimesEntered) const noexcept {
        return waitForTimesEntered(Interruptible::notInterruptible(), targetTimesEntered);
    }

    /**
     * Like `waitForTimesEntered`, but interruptible via the `interruptible->sleepFor` mechanism.
     * See `mongo::Interruptible::sleepFor`.
     */
    EntryCountT waitForTimesEntered(Interruptible* interruptible,
                                    EntryCountT targetTimesEntered) const {
        return _impl()->waitForTimesEntered(interruptible, targetTimesEntered);
    }

    /**
     * @returns a BSON object showing the current mode and data stored.
     */
    BSONObj toBSON() const {
        return _impl()->toBSON();
    }

    /**
     * Create a LockHandle from this FailPoint.
     * The returned object will be active if the failpoint is active.
     * If it's active, the returned object can be used to access FailPoint data.
     */
    LockHandle scoped() {
        return scopedIf(nullptr);
    }

    /**
     * Create a LockHandle from this FailPoint.
     * If `pred(payload)` is true, then the returned object is active and the
     * FailPoint's activation count is altered (relevant to e.g. the `nTimes` mode). If the
     * predicate is false, an inactive LockHandle is returned and this FailPoint's mode is not
     * modified at all.
     * If it's active, the returned object can be used to access FailPoint data.
     * The `pred` should be callable like a `bool pred(const BSONObj&)`.
     */
    template <typename Pred>
    LockHandle scopedIf(Pred&& pred) {
        return _impl()->tryLock(pred);
    }

    template <typename F>
    void execute(F&& f) {
        return executeIf(f, nullptr);
    }

    /**
     * If `pred(payload)` is true, then `f(payload)` is executed and the FailPoint's
     * activation count is altered (relevant to e.g. the `nTimes` mode). Otherwise, `f`
     * is not executed and this FailPoint's mode is not altered (e.g. `nTimes` isn't
     * consumed).
     * The `pred` should be callable like a `bool pred(const BSONObj&)`.
     */
    template <typename F, typename Pred>
    void executeIf(F&& f, Pred&& pred) {
        auto sfp = scopedIf(pred);
        if (MONGO_unlikely(sfp.isActive())) {
            std::forward<F>(f)(sfp.getData());
        }
    }

    /**
     * Take short `_kWaitGranularity` pauses for as long as the FailPoint is
     * active. Though this makes several accesses to `shouldFail()`, it counts
     * as only one increment in the FailPoint `nTimes` counter.
     */
    void pauseWhileSet() {
        pauseWhileSet(Interruptible::notInterruptible());
    }

    /**
     * Like `pauseWhileSet`, but interruptible via the `interruptible->sleepFor` mechanism.  See
     * `mongo::Interruptible::sleepFor`.
     */
    void pauseWhileSet(Interruptible* interruptible) {
        _impl()->pauseWhileSet(interruptible);
    }

    /**
     * Like `pauseWhileSet`, but will also unpause as soon as the cancellation token is canceled.
     * This method does not generate any cancellation related error by itself, it only waits.
     */
    void pauseWhileSetAndNotCanceled(Interruptible* interruptible, const CancelationToken& token) {
        _impl()->pauseWhileSetAndNotCanceled(interruptible, token);
    }

private:
    const Impl* _rawImpl() const {
        return reinterpret_cast<const Impl*>(&_implStorage);
    }

    Impl* _rawImpl() {
        return const_cast<Impl*>(std::as_const(*this)._rawImpl());  // Reuse const overload
    }

    const Impl* _impl() const {
        // Relaxed: such violations can only occur during single-threaded static initialization.
        invariant(_ready.loadRelaxed(), "Use of uninitialized FailPoint");
        return _rawImpl();
    }

    Impl* _impl() {
        return const_cast<Impl*>(std::as_const(*this)._impl());  // Reuse const overload
    }

    const bool _immortal;

    /**
     * True only when `_impl()` should succeed.
     * We exploit zero-initialization of statics to detect use-before-init.
     */
    AtomicWord<bool> _ready;

    std::aligned_storage_t<sizeof(Impl), alignof(Impl)> _implStorage;
};

class FailPointRegistry {
public:
    FailPointRegistry();

    /**
     * Adds a new fail point to this registry. Duplicate names are not allowed.
     *
     * @return the status code under these circumstances:
     *     OK - if successful.
     *     51006 - if the given name already exists in this registry.
     *     CannotMutateObject - if this registry is already frozen.
     */
    Status add(FailPoint* failPoint);

    /**
     * @return a registered FailPoint, or nullptr if it was not registered.
     */
    FailPoint* find(StringData name) const;

    /**
     * Freezes this registry from being modified.
     */
    void freeze();

    /**
     * Creates a new FailPointServerParameter for each failpoint in the registry. This allows the
     * failpoint to be set on the command line via --setParameter, but is only allowed when
     * running with '--setParameter enableTestCommands=1'.
     */
    void registerAllFailPointsAsServerParameters();

    /**
     * Sets all registered FailPoints to Mode::off. Used primarily during unit test cleanup to
     * reset the state of all FailPoints set by the unit test. Does not prevent FailPoints from
     * being enabled again after.
     */
    void disableAllFailpoints();

private:
    bool _frozen;
    StringMap<FailPoint*> _fpMap;
};

/**
 * A scope guard that enables a named FailPoint on construction and disables it on destruction.
 */
class FailPointEnableBlock {
public:
    explicit FailPointEnableBlock(StringData failPointName);
    FailPointEnableBlock(StringData failPointName, BSONObj data);
    explicit FailPointEnableBlock(FailPoint* failPoint);
    FailPointEnableBlock(FailPoint* failPoint, BSONObj data);
    ~FailPointEnableBlock();

    FailPointEnableBlock(const FailPointEnableBlock&) = delete;
    FailPointEnableBlock& operator=(const FailPointEnableBlock&) = delete;

    // Const access to the underlying FailPoint
    const FailPoint* failPoint() const {
        return _failPoint;
    }

    // Const access to the underlying FailPoint
    const FailPoint* operator->() const {
        return failPoint();
    }

    // Return the value of timesEntered() when the block was entered
    auto initialTimesEntered() const {
        return _initialTimesEntered;
    }

private:
    FailPoint* const _failPoint;
    FailPoint::EntryCountT _initialTimesEntered;
};

/**
 * Set a fail point in the global registry to a given value via BSON
 * @return the number of times the fail point has been entered so far.
 * @throw DBException corresponding to ErrorCodes::FailPointSetFailed if no failpoint
 * called failPointName exists.
 */
FailPoint::EntryCountT setGlobalFailPoint(const std::string& failPointName, const BSONObj& cmdObj);

/**
 * Registration object for FailPoint. Its static-initializer registers FailPoint `fp`
 * into the `globalFailPointRegistry()` under the specified `name`.
 */
class FailPointRegisterer {
public:
    explicit FailPointRegisterer(FailPoint* fp);
};

FailPointRegistry& globalFailPointRegistry();

/**
 * Convenience macro for defining a fail point and registering it.
 * Must be used at namespace scope, not at local (inside a function) or class scope.
 * Never use in header files, only .cpp files.
 */
#define MONGO_FAIL_POINT_DEFINE(fp)                               \
    ::mongo::FailPoint fp(#fp, true); /* An immortal FailPoint */ \
    ::mongo::FailPointRegisterer fp##failPointRegisterer(&fp);


}  // namespace mongo
