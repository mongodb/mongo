/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <cstring>
#include <type_traits>

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace waitable_atomic_details {

/**
 * Notifies either all or at least one waiter on uaddr.
 * Must not error even if uaddr is unmapped as there are some algorithms where that is valid.
 */
void notifyOne(const void* uaddr);
void notifyAll(const void* uaddr);

/**
 * Atomically waits for notifications on uaddr, unless it doesn't hold the value old.
 * Returns false on timeout.
 */
bool waitUntil(const void* uaddr,
               uint32_t old,
               boost::optional<stdx::chrono::system_clock::time_point> deadline);

}  // namespace waitable_atomic_details

/**
 * Extends the AtomicWord interface to include wait functionality with deadline and timeout.
 *
 * This class is very similar to the wait() and notify() functions on std::atomic, with these
 * exceptions:
 *  1) It supports extensions from P2643 including deadlines/timeouts
 *  2) It only supports 4-byte types to ensure a native implementation on linux
 *  3) We don't support customizing the memory order and always use seq_cst for the load.
 *
 * This is a lower-level type than WaitableAtomic. It should only be used if you have a 4-byte type
 * that has built-in tracking of whether there are any waiters. If both of those apply, this can
 * save a redundant atomic RMW in notify and wait. If not, look into the more flexible
 * WaitableAtomic class defined below.
 *
 * Note: We require unique object representations to avoid issues with padding bits and weirdness
 * with floats. If needed, we can lift this restriction using the same compiler-specific mechanisms
 * as std::atomic.
 */
template <typename T>
requires(std::has_unique_object_representations_v<T> && sizeof(T) == 4 &&
         sizeof(AtomicWord<T>) == 4)  //
    class BasicWaitableAtomic : public AtomicWord<T> {
public:
    using AtomicWord<T>::AtomicWord;

    /**
     * Returns this->load() once it doesn't equal old.
     *
     * Like compareAndSwap, this function uses memcmp rather than == to decide if values are equal.
     * If this function blocks, it will only check the value again when one of the notify methods is
     * called. Very cheap to call if the current value differs from old, so callers shouldn't do
     * their own checks before calling.
     *
     * The same as std::atomic::wait(), but returns the loaded value (which will not equal old) as
     * proposed in https://wg21.link/P2643.
     */
    T wait(T old) const {
        return *waitUntil(old, boost::none);
    }

    /**
     * Like wait(), but returns boost::none if deadline expires.
     */
    boost::optional<T> waitUntil(T old, Date_t deadline) const {
        return waitUntil(old, deadline.toSystemTimePoint());
    }

    /**
     * Like wait(), but returns boost::none if optional deadline expires.
     */
    boost::optional<T> waitUntil(
        T old, boost::optional<stdx::chrono::system_clock::time_point> deadline) const {
        // We know that T and AtomicWord<T> are 4 bytes but we should also ensure we are too.
        static_assert(sizeof(*this) == 4);

        while (true) {
            const auto curr = this->load();
            if (memcmp(&curr, &old, 4) != 0)
                return curr;

            // The lower level API only supports uint32_t, but we support any 4-byte type.
            uint32_t currU32;
            memcpy(&currU32, &curr, 4);
            if (!waitable_atomic_details::waitUntil(this, currU32, deadline))
                return {};
        }
    }

    /**
     * Like wait(), but returns boost::none if timeout expires.
     */
    boost::optional<T> waitFor(T old, Nanoseconds timeout) const {
        if (auto curr = this->load(); memcmp(&curr, &old, 4) != 0)
            return curr;
        return waitUntil(old, stdx::chrono::system_clock::now() + timeout.toSystemDuration());
    }

    /**
     * Notifies all threads blocked waiting on the atomic object, equivalent to
     * std::atomic::notify_all.
     *
     * WARNING: This is expensive even if there are no active waiters. Callers should track
     * presence of active waiters unless notify is never called in performance-sensitive contexts.
     */
    void notifyAll() {
        waitable_atomic_details::notifyAll(this);
    }

    /**
     * Notifies at least one thread waiting on the atomic object, equivalent to
     * std::atomic::notify_one. If called N times, even concurrently, will wake at least N threads.
     *
     * WARNING: This is expensive even if there are no active waiters. Callers should track
     * presence of active waiters unless notify is never called in performance-sensitive contexts.
     *
     * WARNING: Be very careful with this function if callers may be waiting with
     * different old values. It is unspecified which waiter will be woken, and if any thread is
     * waiting with the current value of this, they will consume the wakeup but treat it as spurious
     * (since it is) and block again. From the consumer's perspective, it will be as-if the
     * notification was lost.
     *
     * If you know that there is only ever be one waiter, there is no advantage to this over
     * notifyAll(). Prefer notifyAll() in that case, so that notifyOne() is only used when it is
     * important for scalability that only one out of many waiters will be woken.
     */
    void notifyOne() {
        waitable_atomic_details::notifyOne(this);
    }
};

/**
 * Extends the AtomicWord interface to provide wait functionality with deadline and timeout.
 *
 * This is a more flexible and easier to use version of BasicAtomicWaitable. It supports any size T
 * that is supported by our AtomicWord type, and takes care of tracking whether there are any
 * waiters so that notifyAll() is cheap when there aren't.
 *
 * notifyOne() is not supported because we only track whether there are any waiters, not how many
 * there are.
 */

template <typename T,
          // try to put counter (member) and value (base) on same cache line.
          size_t alignment = 2 * std::max(sizeof(T), sizeof(uint32_t))>
class alignas(alignment) WaitableAtomic : public AtomicWord<T> {
public:
    using AtomicWord<T>::AtomicWord;

    /**
     * Returns this->load() once it doesn't equal old.
     *
     * Unlike std::atomic::wait(), this function uses == rather than memcmp to decide if values are
     * equal. Be careful with floats using default comparisons! Because it uses == and never !=,
     * 0.0 and -0.0 are considered the same, while NaNs are never considered the same.
     *
     * If this function blocks, it will only check the value again when one of the notify methods is
     * called. Very cheap to call if the current value differs from old, so callers shouldn't do
     * their own checks before calling.
     */
    T wait(T old) const {
        return *waitUntil(old, boost::none);
    }

    /**
     * Like wait(), but returns boost::none if deadline expires.
     */
    boost::optional<T> waitUntil(T old, Date_t deadline) const {
        return waitUntil(old, deadline.toSystemTimePoint());
    }

    /**
     * Like wait(), but returns boost::none if optional deadline expires.
     */
    boost::optional<T> waitUntil(
        T old, boost::optional<stdx::chrono::system_clock::time_point> deadline) const {
        while (true) {
            // Short story of correctness: we want load 1 and 2 to happen in order, and happen
            // before 3 and 4. This is ensured 1 and 2 being at least acquire. Load 3 can be relaxed
            // because it will still happen after loads 1 and 2, and because it is just doing a
            // pre-flight check of what the kernel will do to skip that expensive step if not
            // necessary.
            //
            // Longer story: That description ignores what happens if load 2 reads a value that
            // wasn't written with release semantics or an RMW that extends a release sequence. If
            // that happens, then load 2 being acquire doesn't have any effect in the C++ memory
            // model (it does in x86 and armv8 though, which still makes it useful). In that case,
            // there is no guarantee that 3 happens after 2. But we don't *really* need that, it is
            // just more efficient because it would allow us to skip a syscall if we see the
            // notification. This algorithm would still be correct if we skipped 3 entirely. We do
            // require that the kernel have appropiate fencing to ensure that the registration of
            // the waiter happens before it loads (4) the value to compare. This is what really
            // ensures correctness. On linux this is explicitly documented. On other platforms it
            // isn't, however they only run on x86 and armv8 which are "other-multicopy-atomic"
            // provide the stronger semantics to acquire/release that are used in the short story
            // above (essentially you can pretend that there is a total order to all writes once
            // issued, but that each thread may reorder its reads and writes unless restricted by
            // barriers). Power does not follow that rule, but we only support power on linux.

            auto oldWaitFlag = _waitFlag.load();  // 1

            // Note: not using curr != old otherwise NaNs would block forever.
            if (auto curr = this->load(); !(curr == old)) {  // 2
                return curr;
            }

            if (oldWaitFlag != _waitFlag.loadRelaxed())  // 3
                continue;

            if (!(oldWaitFlag & 1)) {
                // Set have-waiters bit unless a notification came in since we read it.
                // If another waiter set _waitFlag to the same value we were trying to,
                // don't loop, since that means there hasn't been an actual notification.
                const auto newWaitFlag = oldWaitFlag | 1;
                if (!_waitFlag.compareAndSwap(&oldWaitFlag, newWaitFlag) &&
                    oldWaitFlag != newWaitFlag)
                    continue;

                oldWaitFlag = newWaitFlag;  // pass the updated value to waitUntil()
            }

            // wait until the waitFlag changes or the deadline expires.
            if (!_waitFlag.waitUntil(oldWaitFlag, deadline))  // 4 (the in-kernel load)
                return {};
        }
    }

    /**
     * Like wait(), but returns boost::none if timeout expires.
     */
    boost::optional<T> waitFor(T old, Nanoseconds timeout) const {
        // Note: not using curr != old for correct handling of NaNs.
        if (auto curr = this->load(); !(curr == old))
            return curr;
        return waitUntil(old, stdx::chrono::system_clock::now() + timeout.toSystemDuration());
    }

    /**
     * Notifies all threads blocked waiting on the atomic object. This is cheap to call if there are
     * no active waiters. If there are multiple concurrent callers of notifyAll(), only one needs to
     * do the expensive notification.
     */
    void notifyAll() {
        // The first modification needs to be at least release because a waiter that sees a change
        // in the wait flag must also see a change in the atomic value of our base (which was
        // presumably modified by the caller prior to calling notify). The second modification could
        // be relaxed because it doesn't release any additional changes, and as an RMW, extends the
        // release sequence headed by the first write (and all prior releases up to its position in
        // the modification order).

        // Increment the counter so that waiters will see a new value.
        if ((_waitFlag.fetchAndAdd(2) & 1) == 0)
            return;  // Nobody is waiting for us.

        // Clear the waiter flag. If we are racing with a concurrent call to this method, only the
        // one that clears the flag needs to actually do the notification.
        if ((_waitFlag.fetchAndBitAnd(~1) & 1) == 0)
            return;  // We lost the race. Yay, someone else will do the work for us!

        _waitFlag.notifyAll();
    }

private:
    friend bool hasWaiters_forTest(const WaitableAtomic& wa) {
        return wa._waitFlag.load() & 1;
    }

    // Low bit set indicates that there are waiters that need to be notified. Remaining bits are a
    // count of the number of calls to notifyAll(). This allows using fetchAndAdd(2) to increase the
    // count by 1 without touching the low bit.
    //
    // The actual value of the count doesn't matter, it is just to ensure that each call to
    // notifyAll() results in a new value that wasn't used recently so that waiters have a 4-byte
    // value that they can detect changes in to know whether a notification has come in. In theory,
    // this could result in hangs if exactly 1<<31 (about 2 billion) notifications come in between
    // the check of T and the kernel doing its futex registration stuff. That seems unbelievably
    // improbable, but even then, we would only block until the next notification comes in. We could
    // *still* hang if either there was a long gap after 1<<31 notifications, or exactly 1<<31
    // notifications comes again between checks, but that seems far more improbable that the
    // computer randomly catching on fire, so I don't think it is worth worrying about.
    //
    // This variable is only written to with atomic RMWs, so the release sequences extend to all
    // later values in modification order. In other words, an aquire load that reads value V, is
    // synchronized with not just the operation that wrote V (if it was a release) but also all
    // prior release stores to this variable.
    mutable BasicWaitableAtomic<uint32_t> _waitFlag = 0;
};

}  // namespace mongo
