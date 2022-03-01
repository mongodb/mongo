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
#include "vm/EnvironmentObject.h"
#include "vm/GlobalObject.h"
#include "vm/Iteration.h"

#include "vm/JSContext-inl.h"

inline void JS::Realm::initGlobal(
    js::GlobalObject& global, js::GlobalLexicalEnvironmentObject& lexicalEnv) {
  MOZ_ASSERT(global.realm() == this);
  MOZ_ASSERT(!global_);
  global_.set(&global);
  lexicalEnv_.set(&lexicalEnv);
}

js::GlobalObject* JS::Realm::maybeGlobal() const {
  MOZ_ASSERT_IF(global_, global_->realm() == this);
  return global_;
}

js::GlobalLexicalEnvironmentObject* JS::Realm::unbarrieredLexicalEnvironment()
    const {
  return lexicalEnv_.unbarrieredGet();
}

inline bool JS::Realm::globalIsAboutToBeFinalized() {
  MOZ_ASSERT(zone_->isGCSweeping());
  return global_ && js::gc::IsAboutToBeFinalized(&global_);
}

inline bool JS::Realm::hasLiveGlobal() const {
  js::GlobalObject* global = unsafeUnbarrieredMaybeGlobal();
  return global && !js::gc::IsAboutToBeFinalizedUnbarriered(&global);
}

inline bool JS::Realm::marked() const {
  // Preserve this Realm if it has a live global or if it has been entered (to
  // ensure we don't destroy the Realm while we're allocating its global).
  return hasLiveGlobal() || hasBeenEnteredIgnoringJit();
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

MOZ_ALWAYS_INLINE bool js::ObjectRealm::objectMaybeInIteration(JSObject* obj) {
  MOZ_ASSERT(&ObjectRealm::get(obj) == this);

  // If the list is empty we're not iterating any objects.
  js::NativeIterator* next = enumerators->next();
  if (enumerators == next) {
    return false;
  }

  // If the list contains a single object, check if it's |obj|.
  if (next->next() == enumerators) {
    return next->objectBeingIterated() == obj;
  }

  return true;
}

#endif /* vm_Realm_inl_h */
