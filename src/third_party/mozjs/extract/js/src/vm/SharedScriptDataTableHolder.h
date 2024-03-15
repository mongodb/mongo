/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedScriptDataTableHolder_h
#define vm_SharedScriptDataTableHolder_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include "threading/Mutex.h"   // js::Mutex
#include "vm/SharedStencil.h"  // js::SharedImmutableScriptDataTable

namespace js {

class AutoLockGlobalScriptData {
  static js::Mutex mutex_;

 public:
  AutoLockGlobalScriptData();
  ~AutoLockGlobalScriptData();
};

// A class to provide an access to SharedImmutableScriptDataTable,
// with or without a mutex lock.
//
// js::globalSharedScriptDataTableHolder singleton can be used by any thread,
// and it needs a mutex lock.
//
//   AutoLockGlobalScriptData lock;
//   auto& table = js::globalSharedScriptDataTableHolder::get(lock);
//
// Private SharedScriptDataTableHolder instance can be created for thread-local
// storage, and it can be configured not to require a mutex lock.
//
//   SharedScriptDataTableHolder holder(
//     SharedScriptDataTableHolder::NeedsLock::No);
//   ...
//   auto& table = holder.getWithoutLock();
//
// getMaybeLocked method can be used for both type of instances.
//
//   Maybe<AutoLockGlobalScriptData> lock;
//   auto& table = holder.getMaybeLocked(lock);
//
// Private instance is supposed to be held by the each JSRuntime, including
// both main thread runtime and worker thread runtime, and used in for
// non-helper-thread compilation.
//
// js::globalSharedScriptDataTableHolder singleton is supposed to be used by
// all helper-thread compilation.
class SharedScriptDataTableHolder {
  bool needsLock_ = true;
  js::SharedImmutableScriptDataTable scriptDataTable_;

 public:
  enum class NeedsLock { No, Yes };

  explicit SharedScriptDataTableHolder(NeedsLock needsLock = NeedsLock::Yes)
      : needsLock_(needsLock == NeedsLock::Yes) {}

  js::SharedImmutableScriptDataTable& get(
      const js::AutoLockGlobalScriptData& lock) {
    MOZ_ASSERT(needsLock_);
    return scriptDataTable_;
  }

  js::SharedImmutableScriptDataTable& getWithoutLock() {
    MOZ_ASSERT(!needsLock_);
    return scriptDataTable_;
  }

  js::SharedImmutableScriptDataTable& getMaybeLocked(
      mozilla::Maybe<js::AutoLockGlobalScriptData>& lock) {
    if (needsLock_) {
      lock.emplace();
    }
    return scriptDataTable_;
  }
};

extern SharedScriptDataTableHolder globalSharedScriptDataTableHolder;

} /* namespace js */

#endif /* vm_SharedScriptDataTableHolder_h */
