/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_RealmFuses_h
#define vm_RealmFuses_h

#include "vm/GuardFuse.h"
#include "vm/InvalidatingFuse.h"

namespace js {

class NativeObject;
struct RealmFuses;

// [SMDOC] RealmFuses:
//
// Realm fuses are fuses associated with a specific realm. As a result,
// popFuse for realmFuses has another argument, the set of realmFuses related to
// the fuse being popped. This is used to find any dependent fuses in the realm
// (rather than using the context).
class RealmFuse : public GuardFuse {
 public:
  virtual void popFuse(JSContext* cx, RealmFuses& realmFuses) { popFuse(cx); }

 protected:
  virtual void popFuse(JSContext* cx) override { GuardFuse::popFuse(cx); }
};

class InvalidatingRealmFuse : public InvalidatingFuse {
 public:
  virtual void popFuse(JSContext* cx, RealmFuses& realmFuses);
  virtual bool addFuseDependency(JSContext* cx,
                                 Handle<JSScript*> script) override;

 protected:
  virtual void popFuse(JSContext* cx) override {
    InvalidatingFuse::popFuse(cx);
  }
};

struct OptimizeGetIteratorFuse final : public InvalidatingRealmFuse {
  virtual const char* name() override { return "OptimizeGetIteratorFuse"; }
  virtual bool checkInvariant(JSContext* cx) override;
};

struct PopsOptimizedGetIteratorFuse : public RealmFuse {
  virtual void popFuse(JSContext* cx, RealmFuses& realmFuses) override;
};

struct ArrayPrototypeIteratorFuse final : public PopsOptimizedGetIteratorFuse {
  virtual const char* name() override { return "ArrayPrototypeIteratorFuse"; }
  virtual bool checkInvariant(JSContext* cx) override;
};

struct ArrayPrototypeIteratorNextFuse final
    : public PopsOptimizedGetIteratorFuse {
  virtual const char* name() override {
    return "ArrayPrototypeIteratorNextFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// This fuse covers ArrayIteratorPrototype not having a return property;
// however the fuse doesn't pop if a prototype acquires the return property.
struct ArrayIteratorPrototypeHasNoReturnProperty final
    : public PopsOptimizedGetIteratorFuse {
  virtual const char* name() override {
    return "ArrayIteratorPrototypeHasNoReturnProperty";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// This fuse covers IteratorPrototype not having a return property;
// however the fuse doesn't pop if a prototype acquires the return property.
struct IteratorPrototypeHasNoReturnProperty final
    : public PopsOptimizedGetIteratorFuse {
  virtual const char* name() override {
    return "IteratorPrototypeHasNoReturnProperty";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

struct ArrayIteratorPrototypeHasIteratorProto final
    : public PopsOptimizedGetIteratorFuse {
  virtual const char* name() override {
    return "ArrayIteratorPrototypeHasIteratorProto";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

struct IteratorPrototypeHasObjectProto final
    : public PopsOptimizedGetIteratorFuse {
  virtual const char* name() override {
    return "IteratorPrototypeHasObjectProto";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

struct ObjectPrototypeHasNoReturnProperty final
    : public PopsOptimizedGetIteratorFuse {
  virtual const char* name() override {
    return "ObjectPrototypeHasNoReturnProperty";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

#define FOR_EACH_REALM_FUSE(FUSE)                                        \
  FUSE(OptimizeGetIteratorFuse, optimizeGetIteratorFuse)                 \
  FUSE(ArrayPrototypeIteratorFuse, arrayPrototypeIteratorFuse)           \
  FUSE(ArrayPrototypeIteratorNextFuse, arrayPrototypeIteratorNextFuse)   \
  FUSE(ArrayIteratorPrototypeHasNoReturnProperty,                        \
       arrayIteratorPrototypeHasNoReturnProperty)                        \
  FUSE(IteratorPrototypeHasNoReturnProperty,                             \
       iteratorPrototypeHasNoReturnProperty)                             \
  FUSE(ArrayIteratorPrototypeHasIteratorProto,                           \
       arrayIteratorPrototypeHasIteratorProto)                           \
  FUSE(IteratorPrototypeHasObjectProto, iteratorPrototypeHasObjectProto) \
  FUSE(ObjectPrototypeHasNoReturnProperty, objectPrototypeHasNoReturnProperty)

struct RealmFuses {
  RealmFuses() = default;

#define FUSE(Name, LowerName) Name LowerName{};
  FOR_EACH_REALM_FUSE(FUSE)
#undef FUSE

  void assertInvariants(JSContext* cx) {
// Generate the invariant checking calls.
#define FUSE(Name, LowerName) LowerName.assertInvariant(cx);
    FOR_EACH_REALM_FUSE(FUSE)
#undef FUSE
  }

  // Code Generation Code:
  enum class FuseIndex : uint8_t {
  // Generate Fuse Indexes
#define FUSE(Name, LowerName) Name,
    FOR_EACH_REALM_FUSE(FUSE)
#undef FUSE
        LastFuseIndex
  };

  GuardFuse* getFuseByIndex(FuseIndex index) {
    switch (index) {
      // Return fuses.
#define FUSE(Name, LowerName) \
  case FuseIndex::Name:       \
    return &this->LowerName;
      FOR_EACH_REALM_FUSE(FUSE)
#undef FUSE
      default:
        break;
    }
    MOZ_CRASH("Fuse Not Found");
  }

  DependentScriptGroup fuseDependencies;

  static int32_t fuseOffsets[];
  static const char* fuseNames[];

  static int32_t offsetOfFuseWordRelativeToRealm(FuseIndex index);
  static const char* getFuseName(FuseIndex index);
};

}  // namespace js

#endif
