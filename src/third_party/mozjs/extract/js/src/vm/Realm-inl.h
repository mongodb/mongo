/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Realm_inl_h
#define vm_Realm_inl_h

#include "vm/Realm.h"

#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "vm/GlobalObject.h"

#include "vm/JSContext-inl.h"

inline void JS::Realm::initGlobal(js::GlobalObject& global) {
  MOZ_ASSERT(global.realm() == this);
  MOZ_ASSERT(!global_);
  global_.set(&global);
}

js::GlobalObject* JS::Realm::maybeGlobal() const {
  MOZ_ASSERT_IF(global_, global_->realm() == this);
  return global_;
}

inline bool JS::Realm::hasLiveGlobal() const {
  // The global is swept by traceWeakGlobalEdge when we start sweeping a zone
  // group. This frees the GlobalObjectData, so the realm must live at least as
  // long as the global.
  MOZ_ASSERT_IF(global_, !js::gc::IsAboutToBeFinalized(global_));
  return bool(global_);
}

inline bool JS::Realm::hasInitializedGlobal() const {
  return hasLiveGlobal() && !initializingGlobal_;
}

inline bool JS::Realm::marked() const {
  // The Realm survives in the following cases:
  //  - its global is live
  //  - it has been entered (to ensure we don't destroy the Realm while we're
  //    allocating its global)
  //  - it was allocated after the start of an incremental GC (as there may be
  //    pointers to it from other GC things)
  return hasLiveGlobal() || hasBeenEnteredIgnoringJit() ||
         allocatedDuringIncrementalGC_;
}

/* static */ inline js::ObjectRealm& js::ObjectRealm::get(const JSObject* obj) {
  // Note: obj might be a CCW if we're accessing ObjectRealm::enumerators.
  // CCWs here are fine because we always return the same ObjectRealm for a
  // particular (CCW) object.
  return obj->maybeCCWRealm()->objects_;
}

template <typename T>
js::AutoRealm::AutoRealm(JSContext* cx, const T& target)
    : cx_(cx), origin_(cx->realm()) {
  cx_->enterRealmOf(target);
}

// Protected constructor that bypasses assertions in enterRealmOf.
js::AutoRealm::AutoRealm(JSContext* cx, JS::Realm* target)
    : cx_(cx), origin_(cx->realm()) {
  cx_->enterRealm(target);
}

js::AutoRealm::~AutoRealm() { cx_->leaveRealm(origin_); }

js::AutoFunctionOrCurrentRealm::AutoFunctionOrCurrentRealm(JSContext* cx,
                                                           HandleObject fun) {
  JS::Realm* realm = JS::GetFunctionRealm(cx, fun);
  if (!realm) {
    cx->clearPendingException();
    return;
  }

  // Enter the function's realm.
  ar_.emplace(cx, realm);
}

js::AutoAllocInAtomsZone::AutoAllocInAtomsZone(JSContext* cx)
    : cx_(cx), origin_(cx->realm()) {
  cx_->enterAtomsZone();
}

js::AutoAllocInAtomsZone::~AutoAllocInAtomsZone() {
  cx_->leaveAtomsZone(origin_);
}

js::AutoMaybeLeaveAtomsZone::AutoMaybeLeaveAtomsZone(JSContext* cx)
    : cx_(cx), wasInAtomsZone_(cx->zone() && cx->zone()->isAtomsZone()) {
  if (wasInAtomsZone_) {
    cx_->leaveAtomsZone(nullptr);
  }
}

js::AutoMaybeLeaveAtomsZone::~AutoMaybeLeaveAtomsZone() {
  if (wasInAtomsZone_) {
    cx_->enterAtomsZone();
  }
}

js::AutoRealmUnchecked::AutoRealmUnchecked(JSContext* cx, JS::Realm* target)
    : AutoRealm(cx, target) {}

#endif /* vm_Realm_inl_h */
