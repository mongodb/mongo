/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * This file was copied out of the firefox 38.0.1esr source tree from
 * js/src/vm/PosixNSPR.cpp and modified to use the MongoDB threading
 * primitives.
 *
 * The point of this file is to shim the posix emulation of nspr that Mozilla
 * ships with firefox. We force configuration such that the SpiderMonkey build
 * looks for these symbols and we provide them from within our object code
 * rather than attempting to build it in there's so we can take advantage of
 * the cross platform abstractions that we rely upon.
 */

#include "mongo/platform/basic.h"

#include <array>
#include <js/Utility.h>
#include <vm/PosixNSPR.h>

#include "mongo/stdx/chrono.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/time_support.h"

class nspr::Thread {
    mongo::stdx::thread thread_;
    void (*start)(void* arg);
    void* arg;
    bool joinable;

public:
    Thread(void (*start)(void* arg), void* arg, bool joinable)
        : start(start), arg(arg), joinable(joinable) {}

    static void* ThreadRoutine(void* arg);

    mongo::stdx::thread& thread() {
        return thread_;
    }
};

namespace {
thread_local nspr::Thread* kCurrentThread = nullptr;
}  // namespace

void* nspr::Thread::ThreadRoutine(void* arg) {
    Thread* self = static_cast<Thread*>(arg);
    kCurrentThread = self;
    self->start(self->arg);
    if (!self->joinable)
        js_delete(self);
    return nullptr;
}

namespace mongo {
namespace mozjs {

void PR_BindThread(PRThread* thread) {
    kCurrentThread = thread;
}

PRThread* PR_CreateFakeThread() {
    return new PRThread(nullptr, nullptr, true);
}

void PR_DestroyFakeThread(PRThread* thread) {
    delete thread;
}
}  // namespace mozjs
}  // namespace mongo


// In mozjs-45, js_delete takes a const pointer which is incompatible with std::unique_ptr.
template <class T>
static MOZ_ALWAYS_INLINE void js_delete_nonconst(T* p) {
    if (p) {
        p->~T();
        js_free(p);
    }
}

PRThread* PR_CreateThread(PRThreadType type,
                          void (*start)(void* arg),
                          void* arg,
                          PRThreadPriority priority,
                          PRThreadScope scope,
                          PRThreadState state,
                          uint32_t stackSize) {
    MOZ_ASSERT(type == PR_USER_THREAD);
    MOZ_ASSERT(priority == PR_PRIORITY_NORMAL);

    try {
        // We can't use the nspr allocator to allocate this thread, because under asan we
        // instrument the allocator so that asan can track the pointers correctly. This
        // instrumentation
        // requires that pointers be deleted in the same thread that they were allocated in.
        // The threads created in PR_CreateThread are not always freed in the same thread
        // that they were created in. So, we use the standard allocator here.
        auto t = std::make_unique<nspr::Thread>(start, arg, state != PR_UNJOINABLE_THREAD);

        t->thread() = mongo::stdx::thread(&nspr::Thread::ThreadRoutine, t.get());

        if (state == PR_UNJOINABLE_THREAD) {
            t->thread().detach();
        }

        return t.release();
    } catch (...) {
        return nullptr;
    }
}

PRStatus PR_JoinThread(PRThread* thread) {
    try {
        thread->thread().join();

        delete thread;

        return PR_SUCCESS;
    } catch (...) {
        return PR_FAILURE;
    }
}

PRThread* PR_GetCurrentThread() {
    return kCurrentThread;
}

PRStatus PR_SetCurrentThreadName(const char* name) {
    mongo::setThreadName(name);

    return PR_SUCCESS;
}

namespace {

const size_t MaxTLSKeyCount = 32;
size_t gTLSKeyCount;
thread_local std::array<void*, MaxTLSKeyCount> gTLSArray;

}  // namespace

PRStatus PR_NewThreadPrivateIndex(unsigned* newIndex, PRThreadPrivateDTOR destructor) {
    /*
     * We only call PR_NewThreadPrivateIndex from the main thread, so there's no
     * need to lock the table of TLS keys.
     */
    MOZ_ASSERT(gTLSKeyCount + 1 < MaxTLSKeyCount);

    *newIndex = gTLSKeyCount;
    gTLSKeyCount++;

    return PR_SUCCESS;
}

PRStatus PR_SetThreadPrivate(unsigned index, void* priv) {
    if (index >= gTLSKeyCount)
        return PR_FAILURE;

    gTLSArray[index] = priv;

    return PR_SUCCESS;
}

void* PR_GetThreadPrivate(unsigned index) {
    if (index >= gTLSKeyCount)
        return nullptr;

    return gTLSArray[index];
}

PRStatus PR_CallOnce(PRCallOnceType* once, PRCallOnceFN func) {
    MOZ_CRASH("PR_CallOnce unimplemented");
}

PRStatus PR_CallOnceWithArg(PRCallOnceType* once, PRCallOnceWithArgFN func, void* arg) {
    MOZ_CRASH("PR_CallOnceWithArg unimplemented");
}

class nspr::Lock {
    mongo::stdx::mutex mutex_;

public:
    Lock() {}
    mongo::stdx::mutex& mutex() {
        return mutex_;
    }
};

PRLock* PR_NewLock() {
    return js_new<nspr::Lock>();
}

void PR_DestroyLock(PRLock* lock) {
    js_delete(lock);
}

void PR_Lock(PRLock* lock) {
    lock->mutex().lock();
}

PRStatus PR_Unlock(PRLock* lock) {
    lock->mutex().unlock();

    return PR_SUCCESS;
}

class nspr::CondVar {
    mongo::stdx::condition_variable cond_;
    nspr::Lock* lock_;

public:
    CondVar(nspr::Lock* lock) : lock_(lock) {}
    mongo::stdx::condition_variable& cond() {
        return cond_;
    }
    nspr::Lock* lock() {
        return lock_;
    }
};

PRCondVar* PR_NewCondVar(PRLock* lock) {
    return js_new<nspr::CondVar>(lock);
}

void PR_DestroyCondVar(PRCondVar* cvar) {
    js_delete(cvar);
}

PRStatus PR_NotifyCondVar(PRCondVar* cvar) {
    cvar->cond().notify_one();

    return PR_SUCCESS;
}

PRStatus PR_NotifyAllCondVar(PRCondVar* cvar) {
    cvar->cond().notify_all();

    return PR_SUCCESS;
}

uint32_t PR_MillisecondsToInterval(uint32_t milli) {
    return milli;
}

uint32_t PR_MicrosecondsToInterval(uint32_t micro) {
    return (micro + 999) / 1000;
}

static const uint64_t TicksPerSecond = 1000;
static const uint64_t NanoSecondsInSeconds = 1000000000;
static const uint64_t MicroSecondsInSeconds = 1000000;

uint32_t PR_TicksPerSecond() {
    return TicksPerSecond;
}

PRStatus PR_WaitCondVar(PRCondVar* cvar, uint32_t timeout) {
    if (timeout == PR_INTERVAL_NO_TIMEOUT) {
        try {
            mongo::stdx::unique_lock<mongo::stdx::mutex> lk(cvar->lock()->mutex(),
                                                            mongo::stdx::adopt_lock_t());

            cvar->cond().wait(lk);
            lk.release();

            return PR_SUCCESS;
        } catch (...) {
            return PR_FAILURE;
        }
    } else {
        try {
            mongo::stdx::unique_lock<mongo::stdx::mutex> lk(cvar->lock()->mutex(),
                                                            mongo::stdx::adopt_lock_t());

            cvar->cond().wait_for(lk, mongo::Microseconds(timeout).toSystemDuration());
            lk.release();

            return PR_SUCCESS;
        } catch (...) {
            return PR_FAILURE;
        }
    }
}

int32_t PR_FileDesc2NativeHandle(PRFileDesc* fd) {
    MOZ_CRASH("PR_FileDesc2NativeHandle");
}

PRStatus PR_GetOpenFileInfo(PRFileDesc* fd, PRFileInfo* info) {
    MOZ_CRASH("PR_GetOpenFileInfo");
}

int32_t PR_Seek(PRFileDesc* fd, int32_t offset, PRSeekWhence whence) {
    MOZ_CRASH("PR_Seek");
}

PRFileMap* PR_CreateFileMap(PRFileDesc* fd, int64_t size, PRFileMapProtect prot) {
    MOZ_CRASH("PR_CreateFileMap");
}

void* PR_MemMap(PRFileMap* fmap, int64_t offset, uint32_t len) {
    MOZ_CRASH("PR_MemMap");
}

PRStatus PR_MemUnmap(void* addr, uint32_t len) {
    MOZ_CRASH("PR_MemUnmap");
}

PRStatus PR_CloseFileMap(PRFileMap* fmap) {
    MOZ_CRASH("PR_CloseFileMap");
}
