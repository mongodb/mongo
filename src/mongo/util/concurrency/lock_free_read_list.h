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

#include <limits>
#include <list>
#include <memory>
#include <type_traits>

#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scoped_unlock.h"

namespace mongo {

/**
 * Note: This is a generic list with no dependency to `ServiceContext` or other core types, so it
 * is sitting in a separate header as a generic concurrency utility. However, it is optimized to
 * maintain the list of active `Client` objects, and may not be a drop-in for other use-cases.
 * Please make sure you review its documentation before using it for other use-cases.
 *
 * A synchronized linked list that allows lock-free iteration while serializing all modifications
 * (i.e. `add` and `remove`). The list has a few properties that allows lock-free iteration:
 *  - It is append-only and entires cannot be removed once added to the list. Instead, entries are
 *    logically deleted in response to a `remove` operation, and recycled for future inserts.
 *  - Readers do not block behind writers, but skip over a locked entry. Entries are only
 *    exclusively locked prior to being removed, and this data type assumes readers are not
 *    interested in observing removed entries.
 *  - A new entry is either atomically added as the new head of the list, or replaces a logically
 *    deleted entry atomically. This allows readers to observe new entries lock free.
 *
 * This class supports three operations:
 *  - `add` and `remove`: acquire exclusive write access to the list without blocking readers.
 *    `delete` may need to wait for readers to retire, since an entry can only be (logically)
 *    deleted when no reader has specified an intention to read its content (i.e. `Entry::pointer`).
 *  - `getCursor`: acquires a read-lock on the first valid (i.e. not logically deleted) entry in the
 *    list. If there is such an entry, it returns a cursor pointing to it. Otherwise, it returns an
 *    empty (invalid) cursor.
 *
 * Design Details: Readers and writers synchronize via a waitable atomic on each entry (`readers`).
 * Since the list is append-only, readers can safely access entries without any user-after-free
 * concerns. A write intent (there can only be one since writers have to exclusively lock the list)
 * is specified by setting the highest bit of the atomic.
 */
template <typename T>
class LockFreeReadList {
    class EntryImpl;

public:
    class Entry {};

    /**
     * The primary API to read list entries. Acquires a shared lock on each entry as it walks the
     * list. Once an entry is locked, it is wrapped in `LockedEntry`. So long as `LockedEntry`
     * references a valid list entry, the cursor is valid.
     */
    class Cursor {
    public:
        explicit Cursor(EntryImpl* head) : _lockedEntry(_findAndLockEntry(head)) {}

        T value() const {
            iassert(ErrorCodes::BadValue, "Cursor does not hold a value!", !!_lockedEntry);
            return **_lockedEntry.entry;
        }

        bool next() {
            if (MONGO_likely(!!_lockedEntry)) {
                auto next = _lockedEntry.entry->next();
                _lockedEntry = {};  // Release the locked entry before locking another.
                _lockedEntry = _findAndLockEntry(next);
            }
            return !!_lockedEntry;
        }

        explicit operator bool() const {
            return !!_lockedEntry;
        }

    private:
        /**
         * Holds an entry that is locked for reading and releases the lock upon its destruction.
         */
        struct LockedEntry {
            LockedEntry() = default;

            explicit LockedEntry(EntryImpl* lockedEntry) : entry(lockedEntry) {}

            ~LockedEntry() {
                if (entry) {
                    entry->releaseReadLock();
                }
            }

            LockedEntry(LockedEntry&& other) noexcept : entry{std::exchange(other.entry, {})} {}

            LockedEntry& operator=(LockedEntry&& other) noexcept {
                if (this != &other) {
                    auto tmp{std::move(*this)};
                    entry = std::exchange(other.entry, {});
                }
                return *this;
            }

            explicit operator bool() const {
                return !!entry;
            }

            EntryImpl* entry{};
        };

        /**
         * Finds and locks the next valid entry, if there is any, that is not being modified. Throws
         * `ErrorCodes::TooManyLocks` if attempts to lock an entry with too many readers.
         */
        LockedEntry _findAndLockEntry(EntryImpl* entry) const {
            for (; entry; entry = entry->next()) {
                if (entry->tryAcquiringReadLock()) {
                    return LockedEntry(entry);
                }
            }
            return LockedEntry();
        }

        LockedEntry _lockedEntry;
    };

    ~LockFreeReadList() {
        for (auto current = _head.load(); current; current = current->next()) {
            invariant(current->readers() == 0);
        }
    }

    Entry* add(T data) noexcept {
        stdx::lock_guard lk(_updateMutex);
        if (!_freeList.empty()) {
            // We are just recycling an entry from the free-list, so all we have to do is checking
            // it out and setting its data to the new value through `recycle()`.
            auto entry = _freeList.front();
            _freeList.pop_front();
            entry->recycle(data);
            return entry;
        }

        _allocated.emplace_back(std::make_unique<EntryImpl>(_head.load(), std::move(data)));
        EntryImpl* entry = _allocated.back().get();
        _head.store(entry);

        return entry;
    }

    void remove(Entry* e) noexcept {
        auto entry = static_cast<EntryImpl*>(e);
        stdx::unique_lock lk(_updateMutex);
        entry->markDeletedAndAwaitReaders(lk);
        _freeList.push_back(entry);
    }

    Cursor getCursor() const {
        return Cursor(_head.load());
    }

    void setReaders_forTest(Entry* entry, uint32_t value) const {
        static_cast<EntryImpl*>(entry)->setReaders_forTest(value);
    }

    auto getReaders_forTest(Entry* entry) const {
        return static_cast<EntryImpl*>(entry)->readers();
    }

private:
    /**
     * Represents an entry in the list. The `next` pointer is immutable. `readers` keeps track of
     * the number of active readers, as well as the write intent (if there is any). Write intent is
     * set as the highest bit of `readers`:
     * Bits [0 .. 29] represent the number of readers, allowing up to 2 ^ 30 - 1 concurrent reads.
     * Bit 30 must remain zero and allows rejecting readers when there are too many of them.
     * Bit 31 tracks the write intent.
     *
     * Note that we can fit multiple entries in the same cache line to reduce the memory overhead,
     * but decided to make each entry occupy (at least) one cache line for simplicity. Alternatively
     * and considering `T = Client*`, we could fit six entires in a single cache line, where there
     * are six instances of `data`, but only a single `readers` and a single `next` for the entire
     * cache line. This would not allow some of the optimizations that we have below (e.g. readers
     * skipping over individual invalid entries without locking them), but could save on memory
     * usage.
     */
    class alignas(64) EntryImpl : public Entry {
    public:
        static constexpr uint32_t kWriteIntentMask = 1 << 31;
        static constexpr uint32_t kReadersCountMask = ~kWriteIntentMask;
        static constexpr uint32_t kReadersOverflowMask = 1 << 30;

        EntryImpl(EntryImpl* next, T data) : _readers(0), _data(data), _next(next) {}

        void markDeletedAndAwaitReaders(stdx::unique_lock<stdx::mutex>& lk) {
            if (_readers.loadRelaxed() & kWriteIntentMask) {
                // Another thread is removing this entry, or has already removed it.
                return;
            }

            if (auto readers = _readers.fetchAndBitOr(kWriteIntentMask) | kWriteIntentMask;
                readers & kReadersCountMask) {
                ScopedUnlock guard(lk);
                do {
                    readers = _readers.wait(readers);
                } while (readers & kReadersCountMask);
            }

            // Wait for all readers to retire to make sure they do not observe an empty `_data`.
            _data.storeRelaxed({});
        }

        // Readers can visit this entry again and observe `newData` once the following returns.
        void recycle(T newData) {
            _data.storeRelaxed(newData);
            const auto pre = _readers.fetchAndBitXor(kWriteIntentMask);
            invariant(pre & kWriteIntentMask, "A free entry must have its write intent bit set.");
        }

        bool tryAcquiringReadLock() {
            if (_readers.loadRelaxed() & kWriteIntentMask) {
                // This entry is being modified or it was logically deleted. We retain the write
                // intent on logically deleted entries to allow readers skip them without performing
                // expensive acquire-release operations.
                return false;
            }

            const auto value = _readers.addAndFetch(1);
            if (MONGO_unlikely(value & kReadersOverflowMask)) {
                releaseReadLock();
                iasserted(ErrorCodes::TooManyLocks, "Too many readers have acquired the lock!");
            }

            if (MONGO_unlikely(value & kWriteIntentMask)) {
                releaseReadLock();
                return false;
            }

            // We were able to specify our read intent without noticing any writers, so we can
            // safely go ahead with reading this entry while holding our read-lock.
            return true;
        }

        void releaseReadLock() {
            if (auto post = _readers.fetchAndSubtract(1);
                ((post & kReadersCountMask) == 1) && (post & kWriteIntentMask)) {
                _readers.notifyAll();
            }
        }

        uint32_t readers() const {
            return _readers.load() & kReadersCountMask;
        }

        T operator*() const {
            return _data.load();
        }

        EntryImpl* next() const {
            return _next;
        }

        void setReaders_forTest(uint32_t value) {
            _readers.store(value);
        }

    private:
        WaitableAtomic<uint32_t> _readers;
        AtomicWord<T> _data;
        EntryImpl* const _next;
    };

    stdx::mutex _updateMutex;
    std::list<std::unique_ptr<EntryImpl>> _allocated;  // Maintains a list of all allocated entries.
    std::list<EntryImpl*> _freeList;
    Atomic<EntryImpl*> _head;
};

}  // namespace mongo
