/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "threading/ProtectedData.h"

#include "gc/Heap.h"
#include "vm/HelperThreads.h"
#include "vm/JSContext.h"

namespace js {

#ifdef JS_HAS_PROTECTED_DATA_CHECKS

/* static */ mozilla::Atomic<size_t> AutoNoteSingleThreadedRegion::count(0);

template <AllowedHelperThread Helper>
static inline bool
OnHelperThread()
{
    if (Helper == AllowedHelperThread::IonCompile || Helper == AllowedHelperThread::GCTaskOrIonCompile) {
        if (CurrentThreadIsIonCompiling())
            return true;
    }

    if (Helper == AllowedHelperThread::GCTask || Helper == AllowedHelperThread::GCTaskOrIonCompile) {
        if (TlsContext.get()->performingGC || TlsContext.get()->runtime()->gc.onBackgroundThread())
            return true;
    }

    return false;
}

void
CheckThreadLocal::check() const
{
    JSContext* cx = TlsContext.get();
    MOZ_ASSERT(cx);

    // As for CheckZoneGroup, in a cooperatively scheduled runtime the active
    // thread is permitted access to thread local state for other suspended
    // threads in the same runtime.
    if (cx->isCooperativelyScheduled())
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
    else
        MOZ_ASSERT(id == ThisThread::GetId());
}

template <AllowedHelperThread Helper>
void
CheckActiveThread<Helper>::check() const
{
    // When interrupting a thread on Windows, changes are made to the runtime
    // and active thread's state from another thread while the active thread is
    // suspended. We need a way to mark these accesses as being tantamount to
    // accesses by the active thread. See bug 1323066.
#ifndef XP_WIN
    if (OnHelperThread<Helper>())
        return;

    JSContext* cx = TlsContext.get();
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
#endif // XP_WIN
}

template class CheckActiveThread<AllowedHelperThread::None>;
template class CheckActiveThread<AllowedHelperThread::GCTask>;
template class CheckActiveThread<AllowedHelperThread::IonCompile>;

template <AllowedHelperThread Helper>
void
CheckZoneGroup<Helper>::check() const
{
    if (OnHelperThread<Helper>())
        return;

    JSContext* cx = TlsContext.get();
    if (group) {
        if (group->usedByHelperThread()) {
            MOZ_ASSERT(group->ownedByCurrentThread());
        } else {
            // This check is disabled on windows for the same reason as in
            // CheckActiveThread.
#ifndef XP_WIN
            // In a cooperatively scheduled runtime the active thread is
            // permitted access to all zone groups --- even those it has not
            // entered --- for GC and similar purposes. Since all other
            // cooperative threads are suspended, these accesses are threadsafe
            // if the zone group is not in use by a helper thread.
            //
            // A corollary to this is that suspended cooperative threads may
            // not access anything in a zone group, even zone groups they own,
            // because they're not allowed to interact with the JS API.
            MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
#endif
        }
    } else {
        // |group| will be null for data in the atoms zone. This is protected
        // by the exclusive access lock.
        MOZ_ASSERT(cx->runtime()->currentThreadHasExclusiveAccess());
    }
}

template class CheckZoneGroup<AllowedHelperThread::None>;
template class CheckZoneGroup<AllowedHelperThread::GCTask>;
template class CheckZoneGroup<AllowedHelperThread::IonCompile>;
template class CheckZoneGroup<AllowedHelperThread::GCTaskOrIonCompile>;

template <GlobalLock Lock, AllowedHelperThread Helper>
void
CheckGlobalLock<Lock, Helper>::check() const
{
    if (OnHelperThread<Helper>())
        return;

    switch (Lock) {
      case GlobalLock::GCLock:
        MOZ_ASSERT(TlsContext.get()->runtime()->gc.currentThreadHasLockedGC());
        break;
      case GlobalLock::ExclusiveAccessLock:
        MOZ_ASSERT(TlsContext.get()->runtime()->currentThreadHasExclusiveAccess());
        break;
      case GlobalLock::ScriptDataLock:
        MOZ_ASSERT(TlsContext.get()->runtime()->currentThreadHasScriptDataAccess());
        break;
      case GlobalLock::HelperThreadLock:
        MOZ_ASSERT(HelperThreadState().isLockedByCurrentThread());
        break;
    }
}

template class CheckGlobalLock<GlobalLock::GCLock, AllowedHelperThread::None>;
template class CheckGlobalLock<GlobalLock::ExclusiveAccessLock, AllowedHelperThread::None>;
template class CheckGlobalLock<GlobalLock::ExclusiveAccessLock, AllowedHelperThread::GCTask>;
template class CheckGlobalLock<GlobalLock::ScriptDataLock, AllowedHelperThread::None>;
template class CheckGlobalLock<GlobalLock::HelperThreadLock, AllowedHelperThread::None>;

#endif // JS_HAS_PROTECTED_DATA_CHECKS

} // namespace js
