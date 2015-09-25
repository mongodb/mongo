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

#include "mongo/base/error_codes.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/threadlocal.h"

namespace {
MONGO_TRIVIALLY_CONSTRUCTIBLE_THREAD_LOCAL nspr::Thread* kCurrentThread;
}  // namespace

/**
 * It's unfortunate, but we have to use platform threads here over std::thread because we need
 * specified stack sizes. In particular, we guard stack overflow in spidermonkey with
 * JS_SetNativeStackQuota(), and it's more difficult to figure out the size of a thread, after
 * creation, than it is to just create one with a known value.
 */
#ifdef _WIN32
class nspr::Thread {
    HANDLE thread_;
    void (*start)(void* arg);
    void* arg;
    bool joinable;

public:
    Thread(void (*start)(void* arg), void* arg, uint32_t stackSize, bool joinable)
        : start(start), arg(arg), joinable(joinable) {
        thread_ = CreateThread(nullptr, stackSize, ThreadRoutine, this, 0, nullptr);

        if (thread_ == nullptr) {
            mongo::uasserted(mongo::ErrorCodes::InternalError, "Failed in CreateThread");
        }
    }

    void detach() {
        if (CloseHandle(thread_) == 0) {
            mongo::uasserted(mongo::ErrorCodes::InternalError, "Failed in CloseHandle");
        }
    }

    void join() {
        if (WaitForSingleObject(thread_, INFINITE) == WAIT_FAILED) {
            mongo::uasserted(mongo::ErrorCodes::InternalError, "Failed in WaitForSingleObject");
        }
    }

private:
    static DWORD WINAPI ThreadRoutine(LPVOID arg) {
        Thread* self = static_cast<Thread*>(arg);
        kCurrentThread = self;
        self->start(self->arg);
        if (!self->joinable)
            js_delete(self);
        return 0;
    }
};
#else
class nspr::Thread {
    pthread_t thread_;
    void (*start)(void* arg);
    void* arg;
    bool joinable;

public:
    Thread(void (*start)(void* arg), void* arg, uint32_t stackSize, bool joinable)
        : start(start), arg(arg), joinable(joinable) {
        pthread_attr_t attrs;

        if (pthread_attr_init(&attrs) != 0) {
            mongo::uasserted(mongo::ErrorCodes::InternalError, "Failed in pthread_attr_init");
        }

        if (stackSize) {
            if (pthread_attr_setstacksize(&attrs, stackSize) != 0) {
                mongo::uasserted(mongo::ErrorCodes::InternalError,
                                 "Failed in pthread_attr_setstacksize");
            }
        }

        if (pthread_create(&thread_, &attrs, ThreadRoutine, this) != 0) {
            mongo::uasserted(mongo::ErrorCodes::InternalError, "Failed in pthread_create");
        }
    }

    void detach() {
        if (pthread_detach(thread_) != 0) {
            mongo::uasserted(mongo::ErrorCodes::InternalError, "Failed in pthread_detach");
        }
    }

    void join() {
        if (pthread_join(thread_, nullptr) != 0) {
            mongo::uasserted(mongo::ErrorCodes::InternalError, "Failed in pthread_join");
        }
    }

private:
    static void* ThreadRoutine(void* arg) {
        Thread* self = static_cast<Thread*>(arg);
        kCurrentThread = self;
        self->start(self->arg);
        if (!self->joinable)
            js_delete(self);
        return nullptr;
    }
};
#endif

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
        std::unique_ptr<nspr::Thread, void (*)(nspr::Thread*)> t(
            js_new<nspr::Thread>(start, arg, stackSize, state != PR_UNJOINABLE_THREAD),
            js_delete<nspr::Thread>);

        if (state == PR_UNJOINABLE_THREAD) {
            t->detach();
        }

        return t.release();
    } catch (...) {
        return nullptr;
    }
}

PRStatus PR_JoinThread(PRThread* thread) {
    try {
        thread->join();

        js_delete(thread);

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

static const size_t MaxTLSKeyCount = 32;
static size_t gTLSKeyCount;
namespace {
MONGO_TRIVIALLY_CONSTRUCTIBLE_THREAD_LOCAL std::array<void*, MaxTLSKeyCount> gTLSArray;

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

            cvar->cond().wait_for(lk, mongo::stdx::chrono::microseconds(timeout));
            lk.release();

            return PR_SUCCESS;
        } catch (...) {
            return PR_FAILURE;
        }
    }
}
