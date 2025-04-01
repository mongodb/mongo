/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GuardFuse_h
#define vm_GuardFuse_h

#include "mozilla/Array.h"
#include "mozilla/Assertions.h"

#include <stddef.h>

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;

namespace js {

// [SMDOC] Fuses
// A Fuse is a data structure in memory that asserts some property of the
// system.
//
// A fuse may be popped; this indicates that the property the fuse was asserting
// no longer holds.
//
// There are two models of Fuse:
// 1. Guard fuses are used as guards at various points through the engine to
//    allow dynamic choice of a fallback path vs. fast path. A guard is checked
//    explicitly before each action.
// 2. An Invalidating Fuse instead performs some cleanup actions invalidating
//    Ion compiled under the assumption the fuse is intact.
//
// Fuses are designed to be checked easily for validity -- a single load and
// compare should suffice.
//
//
// In order to try make bugs in Fuses fuzzable, each fuse has a
// `checkInvariants` callback. This callback should return `true` iff the
// invariant the fuse asserts still holds. We can then assert that if a fuse is
// intact, then its invariant should hold. This can be triggered by a shell
// testing function, or (not implemented) triggered on a regular basis ala
// JS_GC_ZEAL.
//
// Fuses are agnostic about how they are popped. To support watching for a few
// operations we expect to be important to fuses, see:
//
//   - Watchtower::watchPropertyModification
//   - Watchtower::watchProtoChange
//   - Watchtower::watchPropertyModification.
//
// and ObjectFlags::HasFuseProperty. As well, see MGuardFuse::aliasSet, which
// should be updated if there is any modification to the possible set of places
// fuses could be popped.
//
// In order to support relationships, the popFuse method is virtual and can be
// overridden by subclasses. See RealmFuses.h for examples of how RealmScoped
// fuses are implemented there through overrides of popFuse.
class GuardFuse {
 public:
  GuardFuse() = default;
  GuardFuse(GuardFuse&&) = delete;

  virtual const char* name() = 0;

  // Basic fuse interface: Takes a JSContext argument for subclasses.
  virtual void popFuse(JSContext* cx) {
    MOZ_ASSERT_IF(fuse_, fuse_ == PoppedFuseValue);
    fuse_ = PoppedFuseValue;
  }

  bool intact() {
    MOZ_ASSERT_IF(fuse_, fuse_ == PoppedFuseValue);
    return fuse_ == 0;
  }

  GuardFuse* self() { return this; }

  // Code-Generation Fuse interface
  size_t* fuseRef() { return &fuse_; }
  static int32_t fuseOffset() { return offsetof(GuardFuse, fuse_); }

  // Invariant Maintenance Interface: If an invariant doesn't hold, we should
  // crash the process.
  //
  // Since a fuse is a statement about an invariant in the system, if the fuse
  // is intact, the invariant must hold. However, the converse is not true:
  // a fuse may be popped while its invariant still holds (and for testing
  // purposes we explicitly require this; see popAllFusesInRealm).
  virtual void assertInvariant(JSContext* cx) {
    if (intact()) {
      if (!checkInvariant(cx)) {
        fprintf(stderr, "Fuse %s failed invariant check\n", name());
        MOZ_CRASH("Failed invariant check");
      }
    }
  }

  // Return true iff the invariant asserted by a particular fuse holds; this can
  // safely return false if the fuse is popped.
  virtual bool checkInvariant(JSContext* cx) = 0;

 private:
  // Use a bit pattern to mark a popped fuse -- May be useful in the future for
  // disambiguating between a real popped fuse and a bit-flip.
  static constexpr size_t PoppedFuseValue = 0x808;

  size_t fuse_ = 0;
};

}  // namespace js
#endif  // vm_GuardFuse_h
