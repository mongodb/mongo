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

// Fuse guarding against changes to `Array.prototype[@@iterator]` and
// `%ArrayIteratorPrototype%` that affect the iterator protocol for packed
// arrays.
//
// Popped when one of the following fuses is popped:
// - ArrayPrototypeIteratorFuse (for `Array.prototype[@@iterator]`)
// - OptimizeArrayIteratorPrototypeFuse (for `%ArrayIteratorPrototype%`)
struct OptimizeGetIteratorFuse final : public InvalidatingRealmFuse {
  virtual const char* name() override { return "OptimizeGetIteratorFuse"; }
  virtual bool checkInvariant(JSContext* cx) override;
  virtual void popFuse(JSContext* cx, RealmFuses& realmFuses) override;
};

struct PopsOptimizedGetIteratorFuse : public RealmFuse {
  virtual void popFuse(JSContext* cx, RealmFuses& realmFuses) override;
};

// Fuse guarding against changes to `%ArrayIteratorPrototype%` (and its
// prototype chain) that affect the iterator protocol.
//
// Popped when one of the following fuses is popped:
// - ArrayPrototypeIteratorNextFuse
// - ArrayIteratorPrototypeHasNoReturnProperty
// - ArrayIteratorPrototypeHasIteratorProto
// - IteratorPrototypeHasNoReturnProperty
// - IteratorPrototypeHasObjectProto
// - ObjectPrototypeHasNoReturnProperty
struct OptimizeArrayIteratorPrototypeFuse final
    : public PopsOptimizedGetIteratorFuse {
  virtual const char* name() override {
    return "OptimizeArrayIteratorPrototypeFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

struct PopsOptimizedArrayIteratorPrototypeFuse : public RealmFuse {
  virtual void popFuse(JSContext* cx, RealmFuses& realmFuses) override;
};

struct ArrayPrototypeIteratorFuse final : public PopsOptimizedGetIteratorFuse {
  virtual const char* name() override { return "ArrayPrototypeIteratorFuse"; }
  virtual bool checkInvariant(JSContext* cx) override;
};

struct ArrayPrototypeIteratorNextFuse final
    : public PopsOptimizedArrayIteratorPrototypeFuse {
  virtual const char* name() override {
    return "ArrayPrototypeIteratorNextFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// This fuse covers ArrayIteratorPrototype not having a return property;
// however the fuse doesn't pop if a prototype acquires the return property.
struct ArrayIteratorPrototypeHasNoReturnProperty final
    : public PopsOptimizedArrayIteratorPrototypeFuse {
  virtual const char* name() override {
    return "ArrayIteratorPrototypeHasNoReturnProperty";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// This fuse covers IteratorPrototype not having a return property;
// however the fuse doesn't pop if a prototype acquires the return property.
struct IteratorPrototypeHasNoReturnProperty final
    : public PopsOptimizedArrayIteratorPrototypeFuse {
  virtual const char* name() override {
    return "IteratorPrototypeHasNoReturnProperty";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

struct ArrayIteratorPrototypeHasIteratorProto final
    : public PopsOptimizedArrayIteratorPrototypeFuse {
  virtual const char* name() override {
    return "ArrayIteratorPrototypeHasIteratorProto";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

struct IteratorPrototypeHasObjectProto final
    : public PopsOptimizedArrayIteratorPrototypeFuse {
  virtual const char* name() override {
    return "IteratorPrototypeHasObjectProto";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

struct ObjectPrototypeHasNoReturnProperty final
    : public PopsOptimizedArrayIteratorPrototypeFuse {
  virtual const char* name() override {
    return "ObjectPrototypeHasNoReturnProperty";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// Fuse used to optimize @@species lookups for arrays. If this fuse is intact,
// the following invariants must hold:
//
// - The builtin `Array.prototype` object has a `constructor` property that's
//   the builtin `Array` constructor.
// - This `Array` constructor has a `Symbol.species` property that's the
//   original accessor.
struct OptimizeArraySpeciesFuse final : public InvalidatingRealmFuse {
  virtual const char* name() override { return "OptimizeArraySpeciesFuse"; }
  virtual bool checkInvariant(JSContext* cx) override;
  virtual void popFuse(JSContext* cx, RealmFuses& realmFuses) override;
};

// Fuse used to optimize @@species lookups for ArrayBuffers. If this fuse is
// intact, the following invariants must hold:
//
// - The builtin `ArrayBuffer.prototype` object has a `constructor` property
//   that's the builtin `ArrayBuffer` constructor.
// - This `ArrayBuffer` constructor has a `Symbol.species` property that's the
//   original accessor.
struct OptimizeArrayBufferSpeciesFuse final : public RealmFuse {
  virtual const char* name() override {
    return "OptimizeArrayBufferSpeciesFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// Fuse used to optimize @@species lookups for SharedArrayBuffers. If this fuse
// is intact, the following invariants must hold:
//
// - The builtin `SharedArrayBuffer.prototype` object has a `constructor`
//   property that's the builtin `SharedArrayBuffer` constructor.
// - This `SharedArrayBuffer` constructor has a `Symbol.species` property that's
//   the original accessor.
struct OptimizeSharedArrayBufferSpeciesFuse final : public RealmFuse {
  virtual const char* name() override {
    return "OptimizeSharedArrayBufferSpeciesFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// Fuse used to optimize various property lookups for promises. If this fuse is
// intact, the following invariants must hold:
//
// - The builtin `Promise.prototype` object has unchanged `constructor` and
//   `then` properties.
// - The builtin `Promise` constructor has unchanged `Symbol.species` and
//   `resolve` properties.
struct OptimizePromiseLookupFuse final : public RealmFuse {
  virtual const char* name() override { return "OptimizePromiseLookupFuse"; }
  virtual bool checkInvariant(JSContext* cx) override;
  virtual void popFuse(JSContext* cx, RealmFuses& realmFuses) override;
};

// Fuse used to guard against changes to various properties on RegExp.prototype.
//
// If this fuse is intact, RegExp.prototype must have the following original
// getter properties:
// - .flags ($RegExpFlagsGetter)
// - .global (regexp_global)
// - .hasIndices (regexp_hasIndices)
// - .ignoreCase (regexp_ignoreCase)
// - .multiline (regexp_multiline)
// - .sticky (regexp_sticky)
// - .unicode (regexp_unicode)
// - .unicodeSets (regexp_unicodeSets)
// - .dotAll (regexp_dotAll)
//
// And the following unchanged data properties:
// - .exec (RegExp_prototype_Exec)
// - [@@match] (RegExpMatch)
// - [@@matchAll] (RegExpMatchAll)
// - [@@replace] (RegExpReplace)
// - [@@search] (RegExpSearch)
// - [@@split] (RegExpSplit)
struct OptimizeRegExpPrototypeFuse final : public InvalidatingRealmFuse {
  virtual const char* name() override { return "OptimizeRegExpPrototypeFuse"; }
  virtual bool checkInvariant(JSContext* cx) override;
};

// Fuse used to optimize lookups of certain symbols on String.prototype.
// If this fuse is intact, the following invariants must hold:
//
// - The builtin String.prototype object has the builtin Object.prototype object
//   as prototype.
// - Both String.prototype and Object.prototype don't have any of the following
//   properties: Symbol.match, Symbol.replace, Symbol.search, Symbol.split.
struct OptimizeStringPrototypeSymbolsFuse final : public InvalidatingRealmFuse {
  virtual const char* name() override {
    return "OptimizeStringPrototypeSymbolsFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// Guard used to optimize iterating over Map objects. If this fuse is intact,
// the following invariants must hold:
//
// - The builtin `Map.prototype` object has a `Symbol.iterator` property that's
//   the original `%Map.prototype.entries%` function.
// - The builtin `%MapIteratorPrototype%` object has a `next` property that's
//   the original `MapIteratorNext` self-hosted function.
//
// Note: because this doesn't guard against `return` properties on the iterator
// prototype, this should only be used in places where we don't have to call
// `IteratorClose`.
struct OptimizeMapObjectIteratorFuse final : public RealmFuse {
  virtual const char* name() override {
    return "OptimizeMapObjectIteratorFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// Guard used to optimize iterating over Set objects. If this fuse is intact,
// the following invariants must hold:
//
// - The builtin `Set.prototype` object has a `Symbol.iterator` property that's
//   the original `%Set.prototype.values%` function.
// - The builtin `%SetIteratorPrototype%` object has a `next` property that's
//   the original `SetIteratorNext` self-hosted function.
//
// Note: because this doesn't guard against `return` properties on the iterator
// prototype, this should only be used in places where we don't have to call
// `IteratorClose`.
struct OptimizeSetObjectIteratorFuse final : public RealmFuse {
  virtual const char* name() override {
    return "OptimizeSetObjectIteratorFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// This fuse is popped when the `Map.prototype.set` property is mutated.
struct OptimizeMapPrototypeSetFuse final : public RealmFuse {
  virtual const char* name() override { return "OptimizeMapPrototypeSetFuse"; }
  virtual bool checkInvariant(JSContext* cx) override;
};

// This fuse is popped when the `Set.prototype.add` property is mutated.
struct OptimizeSetPrototypeAddFuse final : public RealmFuse {
  virtual const char* name() override { return "OptimizeSetPrototypeAddFuse"; }
  virtual bool checkInvariant(JSContext* cx) override;
};

// This fuse is popped when the `WeakMap.prototype.set` property is mutated.
struct OptimizeWeakMapPrototypeSetFuse final : public RealmFuse {
  virtual const char* name() override {
    return "OptimizeWeakMapPrototypeSetFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

// This fuse is popped when the `WeakSet.prototype.add` property is mutated.
struct OptimizeWeakSetPrototypeAddFuse final : public RealmFuse {
  virtual const char* name() override {
    return "OptimizeWeakSetPrototypeAddFuse";
  }
  virtual bool checkInvariant(JSContext* cx) override;
};

#define FOR_EACH_REALM_FUSE(FUSE)                                              \
  FUSE(OptimizeGetIteratorFuse, optimizeGetIteratorFuse)                       \
  FUSE(OptimizeArrayIteratorPrototypeFuse, optimizeArrayIteratorPrototypeFuse) \
  FUSE(ArrayPrototypeIteratorFuse, arrayPrototypeIteratorFuse)                 \
  FUSE(ArrayPrototypeIteratorNextFuse, arrayPrototypeIteratorNextFuse)         \
  FUSE(ArrayIteratorPrototypeHasNoReturnProperty,                              \
       arrayIteratorPrototypeHasNoReturnProperty)                              \
  FUSE(IteratorPrototypeHasNoReturnProperty,                                   \
       iteratorPrototypeHasNoReturnProperty)                                   \
  FUSE(ArrayIteratorPrototypeHasIteratorProto,                                 \
       arrayIteratorPrototypeHasIteratorProto)                                 \
  FUSE(IteratorPrototypeHasObjectProto, iteratorPrototypeHasObjectProto)       \
  FUSE(ObjectPrototypeHasNoReturnProperty, objectPrototypeHasNoReturnProperty) \
  FUSE(OptimizeArraySpeciesFuse, optimizeArraySpeciesFuse)                     \
  FUSE(OptimizeArrayBufferSpeciesFuse, optimizeArrayBufferSpeciesFuse)         \
  FUSE(OptimizeSharedArrayBufferSpeciesFuse,                                   \
       optimizeSharedArrayBufferSpeciesFuse)                                   \
  FUSE(OptimizePromiseLookupFuse, optimizePromiseLookupFuse)                   \
  FUSE(OptimizeRegExpPrototypeFuse, optimizeRegExpPrototypeFuse)               \
  FUSE(OptimizeStringPrototypeSymbolsFuse, optimizeStringPrototypeSymbolsFuse) \
  FUSE(OptimizeMapObjectIteratorFuse, optimizeMapObjectIteratorFuse)           \
  FUSE(OptimizeSetObjectIteratorFuse, optimizeSetObjectIteratorFuse)           \
  FUSE(OptimizeMapPrototypeSetFuse, optimizeMapPrototypeSetFuse)               \
  FUSE(OptimizeSetPrototypeAddFuse, optimizeSetPrototypeAddFuse)               \
  FUSE(OptimizeWeakMapPrototypeSetFuse, optimizeWeakMapPrototypeSetFuse)       \
  FUSE(OptimizeWeakSetPrototypeAddFuse, optimizeWeakSetPrototypeAddFuse)

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

#ifdef DEBUG
  static bool isInvalidatingFuse(FuseIndex index) {
    switch (index) {
#  define FUSE(Name, LowerName)                                      \
    case FuseIndex::Name:                                            \
      static_assert(std::is_base_of_v<RealmFuse, Name> ||            \
                    std::is_base_of_v<InvalidatingRealmFuse, Name>); \
      return std::is_base_of_v<InvalidatingRealmFuse, Name>;
      FOR_EACH_REALM_FUSE(FUSE)
#  undef FUSE
      default:
        break;
    }
    MOZ_CRASH("Fuse Not Found");
  }
#endif
};

}  // namespace js

#endif
