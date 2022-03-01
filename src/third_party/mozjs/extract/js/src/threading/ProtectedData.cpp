/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "threading/ProtectedData.h"

#include "gc/Zone.h"
#include "vm/HelperThreads.h"
#include "vm/JSContext.h"

namespace js {

#ifdef JS_HAS_PROTECTED_DATA_CHECKS

/* static */ mozilla::Atomic<size_t, mozilla::SequentiallyConsistent>
    AutoNoteSingleThreadedRegion::count(0);

template <AllowedHelperThread Helper>
static inline bool OnHelperThread() {
  if (Helper == AllowedHelperThread::IonCompile ||
      Helper == AllowedHelperThread::GCTaskOrIonCompile) {
    if (CurrentThreadIsIonCompiling()) {
      return true;
    }
  }

  if (Helper == AllowedHelperThread::GCTask ||
      Helper == AllowedHelperThread::GCTaskOrIonCompile) {
    JSContext* cx = TlsContext.get();
    if (cx->defaultFreeOp()->isCollecting()) {
      return true;
    }
  }

  return false;
}

void CheckThreadLocal::check() const {
  JSContext* cx = TlsContext.get();
  MOZ_ASSERT(cx);
  MOZ_ASSERT_IF(cx->isMainThreadContext(),
                CurrentThreadCanAccessRuntime(cx->runtime()));
  MOZ_ASSERT(id == ThreadId::ThisThreadId());
}

void CheckContextLocal::check() const {
  if (!cx_->isInitialized()) {
    return;
  }

  JSContext* cx = TlsContext.get();
  MOZ_ASSERT(cx);
  MOZ_ASSERT_IF(cx->isMainThreadContext(),
                CurrentThreadCanAccessRuntime(cx->runtime()));
  MOZ_ASSERT(cx_ == cx);
}

template <AllowedHelperThread Helper>
void CheckMainThread<Helper>::check() const {
  if (OnHelperThread<Helper>()) {
    return;
  }

  JSContext* cx = TlsContext.get();
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
}

template class CheckMainThread<AllowedHelperThread::None>;
template class CheckMainThread<AllowedHelperThread::GCTask>;
template class CheckMainThread<AllowedHelperThread::IonCompile>;

template <AllowedHelperThread Helper>
void CheckZone<Helper>::check() const {
  if (OnHelperThread<Helper>()) {
    return;
  }

  if (zone->usedByHelperThread()) {
    // This may only be accessed by the helper thread using this zone.
    MOZ_ASSERT(zone->ownedByCurrentHelperThread());
  } else {
    // The main thread is permitted access to all zones. These accesses
    // are threadsafe if the zone is not in use by a helper thread.
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(TlsContext.get()->runtime()));
  }
}

template class CheckZone<AllowedHelperThread::None>;
template class CheckZone<AllowedHelperThread::GCTask>;
template class CheckZone<AllowedHelperThread::IonCompile>;
template class CheckZone<AllowedHelperThread::GCTaskOrIonCompile>;

template <GlobalLock Lock, AllowedHelperThread Helper>
void CheckGlobalLock<Lock, Helper>::check() const {
  if (OnHelperThread<Helper>()) {
    return;
  }

  switch (Lock) {
    case GlobalLock::GCLock:
      TlsContext.get()->runtime()->gc.assertCurrentThreadHasLockedGC();
      break;
    case GlobalLock::ScriptDataLock:
      TlsContext.get()->runtime()->assertCurrentThreadHasScriptDataAccess();
      break;
    case GlobalLock::HelperThreadLock:
      gHelperThreadLock.assertOwnedByCurrentThread();
      break;
  }
}

template class CheckGlobalLock<GlobalLock::GCLock, AllowedHelperThread::None>;
template class CheckGlobalLock<GlobalLock::ScriptDataLock,
                               AllowedHelperThread::None>;
template class CheckGlobalLock<GlobalLock::HelperThreadLock,
                               AllowedHelperThread::None>;

template <AllowedHelperThread Helper>
void CheckArenaListAccess<Helper>::check() const {
  MOZ_ASSERT(zone);

  if (OnHelperThread<Helper>()) {
    return;
  }

  JSRuntime* rt = TlsContext.get()->runtime();
  if (zone->isAtomsZone()) {
    // The main thread can access the atoms arenas if it holds all the atoms
    // table locks.
    if (rt->currentThreadHasAtomsTableAccess()) {
      return;
    }

    // Otherwise we must hold the GC lock if parallel parsing is running.
    if (rt->isOffThreadParseRunning()) {
      rt->gc.assertCurrentThreadHasLockedGC();
    }
    return;
  }

  CheckZone<AllowedHelperThread::None>::check();
}

template class CheckArenaListAccess<AllowedHelperThread::GCTask>;

#endif  // JS_HAS_PROTECTED_DATA_CHECKS

}  // namespace js
