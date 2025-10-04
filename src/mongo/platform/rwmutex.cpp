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

#include "mongo/platform/rwmutex.h"

#include "mongo/platform/compiler.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/static_immortal.h"

#include <algorithm>
#include <array>
#include <forward_list>
#include <functional>
#include <memory>

#include <boost/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

/**
 * While checked out from the registry (see `LockEntryRegistry`), this will hold the address of the
 * `WriteRarelyRWMutex` that the thread holds a read lock on. For simplicity, this may only hold a
 * single read lock. However, this design can be extended to support arbitrary number of read locks.
 */
struct alignas(64) LockEntry {
    /**
     * As future work, and once we need to allow threads hold on to more read locks, we can use a
     * single `WaitableAtomic` to notify writers (once a lock is released) and fit more entires in a
     * single cache-line.
     */
    WaitableAtomic<WriteRarelyRWMutex*> entry;
};

/**
 * This is basically the allocator for lock entries, and aims to arrange them so that sequentially
 * scanning the thread-local locks is fast and efficient. Entries are allocated on demand, and never
 * freed except for test-only purposes where they are all freed at once upon destruction of the
 * registry. This type is used to define a global registry.
 */
class LockEntryRegistry {
public:
    LockEntry* checkout() {
        stdx::lock_guard lk(_mutex);
        if (MONGO_unlikely(_freeList.empty())) {
            _refill(lk);
        }

        auto lockEntry = _freeList.front();
        _freeList.pop_front();
        ++_checkedOut;
        return lockEntry;
    }

    void release(LockEntry* lockEntry) {
        invariant(lockEntry->entry.load() == nullptr,
                  "Thread destroyed while holding on to a WriteRarelyRWMutex read lock");
        stdx::lock_guard lk(_mutex);
        _freeList.push_front(lockEntry);
        --_checkedOut;
    }

    template <typename CallbackType>
    inline void visitLocks(CallbackType callback) {
        for (auto bh = _blockHoldersHead.load(); bh; bh = bh->next) {
            for (auto& lockEntry : bh->block->entries) {
                callback(lockEntry.entry);
            }
        }
    }

    void reset_forTest() {
        stdx::lock_guard lk(_mutex);
        invariant(_checkedOut == 0, "Cannot reset a `LockEntryRegistry` with active leases!");

        _freeList.clear();
        while (auto current = _blockHoldersHead.load()) {
            std::unique_ptr<BlockHolder> toDelete(current);
            _blockHoldersHead.store(toDelete->next);
        }
    }

private:
    static_assert(sizeof(LockEntry) == 64);
    static constexpr size_t kMemBlockSize = 4096;
    static constexpr size_t kEntriesPerBlock = kMemBlockSize / sizeof(LockEntry);

    struct alignas(kMemBlockSize) MemoryBlock {
        std::array<LockEntry, kEntriesPerBlock> entries;
    };
    static_assert(sizeof(MemoryBlock) == kMemBlockSize);

    struct BlockHolder {
        BlockHolder* next = nullptr;
        std::unique_ptr<MemoryBlock> block;
    };

    MONGO_COMPILER_NOINLINE void _refill(WithLock) {
        auto bh = std::make_unique<BlockHolder>();
        // The following assumes each `LockList` is zeroed out by its ctor.
        bh->block = std::make_unique<MemoryBlock>();
        for (auto& entry : bh->block->entries) {
            _freeList.push_front(&entry);
        }

        bh->next = _blockHoldersHead.load();
        _blockHoldersHead.store(bh.release());
    }

    stdx::mutex _mutex;
    std::forward_list<LockEntry*> _freeList;

    // Debug-only counter that tracks the number of checked-out instances of `LockEntry`.
    int _checkedOut = 0;

    /**
     * The head of a singly-linked-list of allocated blocks, which may only be modified while
     * holding a lock on `_mutex`. The blocks are never deleted, except for test purposes.
     */
    Atomic<BlockHolder*> _blockHoldersHead;
};

static LockEntryRegistry& globalLockRegistry() {
    static StaticImmortal<LockEntryRegistry> registry;
    return *registry;
}

class LockEntryHandle {
public:
    LockEntryHandle() = default;

    ~LockEntryHandle() {
        _reset();
    }

    MONGO_COMPILER_NOINLINE auto initialize() {
        _lockEntry = globalLockRegistry().checkout();
        return _lockEntry;
    }

    auto& operator*() noexcept {
        return _lockEntry->entry;
    }

private:
    void _reset() {
        if (_lockEntry) {
            globalLockRegistry().release(_lockEntry);
            _lockEntry = nullptr;
        }
    }

    friend void resetForTest(LockEntryHandle& handle) {
        handle._reset();
    }

    LockEntry* _lockEntry{};
};

thread_local LockEntryHandle myLockHandle;
thread_local constinit LockEntry* myLockEntry = nullptr;

MONGO_COMPILER_NOINLINE MONGO_COMPILER_COLD_FUNCTION LockEntry* setupThreadLockEntry() {
    return (myLockEntry = myLockHandle.initialize());
}

static constexpr int kRaisedWriteFlag = 1;  // Value indicating an active writer.

}  // namespace

void WriteRarelyRWMutex::_lock() {
    _writeMutex.lock();
    invariant(_writeFlag.load() == 0);
    _writeFlag.store(kRaisedWriteFlag);
    // Beyond this point, new readers will notice the write intent and forgo their shared lock.
    globalLockRegistry().visitLocks([&](auto& entry) {
        if (auto value = entry.load(); MONGO_unlikely(value == this)) {
            // Wait for the reader to retire and notify this thread.
            entry.wait(this);
        }
    });
}

void WriteRarelyRWMutex::_unlock() {
    invariant(_writeFlag.load() == kRaisedWriteFlag);
    _writeFlag.store(0);
    _writeFlag.notifyAll();
    _writeMutex.unlock();
}

MONGO_COMPILER_NOINLINE void WriteRarelyRWMutex::_releaseSharedLockAndWaitForWriter() {
    // Readers should await completion of the write, instead of spinning. We may want to change how
    // readers wait to also check for interruptions while waiting, if needed.
    _unlock_shared();
    _writeFlag.wait(kRaisedWriteFlag);
}

void WriteRarelyRWMutex::_lock_shared() {
    auto lockEntry = myLockEntry;
    if (MONGO_unlikely(!lockEntry)) {
        lockEntry = setupThreadLockEntry();
    }

    auto& entry = lockEntry->entry;
    invariant(entry.loadRelaxed() == nullptr,
              "Attempted to acquire more than one read-write mutex at once");

    while (true) {
        entry.store(this);
        // This load establishes the acquire ordering for the mutex.
        if (MONGO_unlikely(_writeFlag.load())) {
            _releaseSharedLockAndWaitForWriter();
            continue;
        }

        // No write intent was set while the current thread added this mutex to its local mutex
        // list, so any future writers will observe this shared lock acquisition.
        return;
    }
}

void WriteRarelyRWMutex::_unlock_shared() {
    auto& entry = myLockEntry->entry;
    invariant(entry.loadRelaxed() == this,
              "Attempted to unlock a WriteRarelyRWMutex not held by this thread");

    // This store establishes the release ordering for the mutex.
    entry.store(nullptr);
    if (MONGO_unlikely(_writeFlag.load())) {
        // A writer could be waiting on this reader to retire and release its shared lock.
        entry.notifyAll();
    }
}

namespace write_rarely_rwmutex_details {

void resetGlobalLockRegistry_forTest() {
    resetForTest(myLockHandle);
    myLockEntry = nullptr;
    globalLockRegistry().reset_forTest();
}

}  // namespace write_rarely_rwmutex_details

}  // namespace mongo
