/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCHelperState_h
#define gc_GCHelperState_h

#include "js/TypeDecls.h"
#include "threading/ConditionVariable.h"
#include "threading/ProtectedData.h"

namespace js {
class AutoLockHelperThreadState;

namespace gc {
class ArenaLists;
} /* namespace gc */

/*
 * Helper state for use when JS helper threads sweep and allocate GC thing kinds
 * that can be swept and allocated off thread.
 *
 * In non-threadsafe builds, all actual sweeping and allocation is performed
 * on the active thread, but GCHelperState encapsulates this from clients as
 * much as possible.
 */
class GCHelperState
{
    enum State {
        IDLE,
        SWEEPING
    };

    // Associated runtime.
    JSRuntime* const rt;

    // Condvar for notifying the active thread when work has finished. This is
    // associated with the runtime's GC lock --- the worker thread state
    // condvars can't be used here due to lock ordering issues.
    ConditionVariable done;

    // Activity for the helper to do, protected by the GC lock.
    ActiveThreadOrGCTaskData<State> state_;

    // Whether work is being performed on some thread.
    GCLockData<bool> hasThread;

    void startBackgroundThread(State newState, const AutoLockGC& lock,
                               const AutoLockHelperThreadState& helperLock);
    void waitForBackgroundThread(js::AutoLockGC& lock);

    State state(const AutoLockGC&);
    void setState(State state, const AutoLockGC&);

    friend class js::gc::ArenaLists;

    static void freeElementsAndArray(void** array, void** end) {
        MOZ_ASSERT(array <= end);
        for (void** p = array; p != end; ++p)
            js_free(*p);
        js_free(array);
    }

    void doSweep(AutoLockGC& lock);

  public:
    explicit GCHelperState(JSRuntime* rt)
      : rt(rt),
        done(),
        state_(IDLE)
    { }

    JSRuntime* runtime() { return rt; }

    void finish();

    void work();

    void maybeStartBackgroundSweep(const AutoLockGC& lock,
                                   const AutoLockHelperThreadState& helperLock);
    void startBackgroundShrink(const AutoLockGC& lock);

    /* Must be called without the GC lock taken. */
    void waitBackgroundSweepEnd();

#ifdef DEBUG
    bool onBackgroundThread();
#endif

    /*
     * Outside the GC lock may give true answer when in fact the sweeping has
     * been done.
     */
    bool isBackgroundSweeping() const {
        return state_ == SWEEPING;
    }
};

} /* namespace js */

#endif /* gc_GCHelperState_h */
