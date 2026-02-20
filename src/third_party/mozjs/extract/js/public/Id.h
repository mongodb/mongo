/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Id_h
#define js_Id_h

// [SMDOC] PropertyKey / jsid
//
// A PropertyKey is an identifier for a property of an object which is either a
// 31-bit unsigned integer, interned string or symbol.
//
// Also, there is an additional PropertyKey value, PropertyKey::Void(), which
// does not occur in JS scripts but may be used to indicate the absence of a
// valid key. A void PropertyKey is not a valid key and only arises as an
// exceptional API return value. Embeddings must not pass a void PropertyKey
// into JSAPI entry points expecting a PropertyKey and do not need to handle
// void keys in hooks receiving a PropertyKey except when explicitly noted in
// the API contract.
//
// A PropertyKey is not implicitly convertible to or from a Value; JS_ValueToId
// or JS_IdToValue must be used instead.
//
// jsid is an alias for JS::PropertyKey. New code should use PropertyKey instead
// of jsid.

#include "mozilla/Maybe.h"

#include "jstypes.h"

#include "js/GCAnnotations.h"
#include "js/HeapAPI.h"
#include "js/RootingAPI.h"
#include "js/TraceKind.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"

namespace js {
class JS_PUBLIC_API GenericPrinter;
class JSONPrinter;
}  // namespace js

namespace JS {

enum class SymbolCode : uint32_t;

class PropertyKey {
  uintptr_t asBits_;

 public:
  // All keys with the low bit set are integer keys. This means the other type
  // tags must all be even. These constants are public only for the JITs.
  static constexpr uintptr_t IntTagBit = 0x1;
  // Use 0 for StringTypeTag to avoid a bitwise op for atom <-> id conversions.
  static constexpr uintptr_t StringTypeTag = 0x0;
  static constexpr uintptr_t VoidTypeTag = 0x2;
  static constexpr uintptr_t SymbolTypeTag = 0x4;
  // (0x6 is unused)
  static constexpr uintptr_t TypeMask = 0x7;

  static constexpr uint32_t IntMin = 0;
  static constexpr uint32_t IntMax = INT32_MAX;

  constexpr PropertyKey() : asBits_(VoidTypeTag) {}

  static constexpr MOZ_ALWAYS_INLINE PropertyKey fromRawBits(uintptr_t bits) {
    PropertyKey id;
    id.asBits_ = bits;
    return id;
  }

  bool operator==(const PropertyKey& rhs) const {
    return asBits_ == rhs.asBits_;
  }
  bool operator!=(const PropertyKey& rhs) const {
    return asBits_ != rhs.asBits_;
  }

  MOZ_ALWAYS_INLINE bool isVoid() const {
    MOZ_ASSERT_IF((asBits_ & TypeMask) == VoidTypeTag, asBits_ == VoidTypeTag);
    return asBits_ == VoidTypeTag;
  }

  MOZ_ALWAYS_INLINE bool isInt() const { return !!(asBits_ & IntTagBit); }

  MOZ_ALWAYS_INLINE bool isString() const {
    return (asBits_ & TypeMask) == StringTypeTag;
  }

  MOZ_ALWAYS_INLINE bool isSymbol() const {
    return (asBits_ & TypeMask) == SymbolTypeTag;
  }

  MOZ_ALWAYS_INLINE bool isGCThing() const { return isString() || isSymbol(); }

  constexpr uintptr_t asRawBits() const { return asBits_; }

  MOZ_ALWAYS_INLINE int32_t toInt() const {
    MOZ_ASSERT(isInt());
    uint32_t bits = static_cast<uint32_t>(asBits_) >> 1;
    return static_cast<int32_t>(bits);
  }

  MOZ_ALWAYS_INLINE JSString* toString() const {
    MOZ_ASSERT(isString());
    // Use XOR instead of `& ~TypeMask` because small immediates can be
    // encoded more efficiently on some platorms.
    return reinterpret_cast<JSString*>(asBits_ ^ StringTypeTag);
  }

  MOZ_ALWAYS_INLINE JS::Symbol* toSymbol() const {
    MOZ_ASSERT(isSymbol());
    return reinterpret_cast<JS::Symbol*>(asBits_ ^ SymbolTypeTag);
  }

  js::gc::Cell* toGCThing() const {
    MOZ_ASSERT(isGCThing());
    return reinterpret_cast<js::gc::Cell*>(asBits_ & ~TypeMask);
  }

  GCCellPtr toGCCellPtr() const {
    js::gc::Cell* thing = toGCThing();
    if (isString()) {
      return JS::GCCellPtr(thing, JS::TraceKind::String);
    }
    MOZ_ASSERT(isSymbol());
    return JS::GCCellPtr(thing, JS::TraceKind::Symbol);
  }

  bool isPrivateName() const;

  bool isWellKnownSymbol(JS::SymbolCode code) const;

  // A void PropertyKey. This is equivalent to a PropertyKey created by the
  // default constructor.
  static constexpr PropertyKey Void() { return PropertyKey(); }

  static constexpr bool fitsInInt(int32_t i) { return i >= 0; }

  static constexpr PropertyKey Int(int32_t i) {
    MOZ_ASSERT(fitsInInt(i));
    uint32_t bits = (static_cast<uint32_t>(i) << 1) | IntTagBit;
    return PropertyKey::fromRawBits(bits);
  }

  static PropertyKey Symbol(JS::Symbol* sym) {
    MOZ_ASSERT(sym != nullptr);
    MOZ_ASSERT((uintptr_t(sym) & TypeMask) == 0);
    MOZ_ASSERT(!js::gc::IsInsideNursery(reinterpret_cast<js::gc::Cell*>(sym)));
    return PropertyKey::fromRawBits(uintptr_t(sym) | SymbolTypeTag);
  }

  // Must not be used on atoms that are representable as integer PropertyKey.
  // Prefer NameToId or AtomToId over this function:
  //
  // A PropertyName is an atom that does not contain an integer in the range
  // [0, UINT32_MAX]. However, PropertyKey can only hold an integer in the range
  // [0, IntMax] (where IntMax == 2^31-1).  Thus, for the range of integers
  // (IntMax, UINT32_MAX], to represent as a 'id', it must be
  // the case id.isString() and id.toString()->isIndex(). In most
  // cases when creating a PropertyKey, code does not have to care about
  // this corner case because:
  //
  // - When given an arbitrary JSAtom*, AtomToId must be used, which checks for
  //   integer atoms representable as integer PropertyKey, and does this
  //   conversion.
  //
  // - When given a PropertyName*, NameToId can be used which does not need
  //   to do any dynamic checks.
  //
  // Thus, it is only the rare third case which needs this function, which
  // handles any JSAtom* that is known not to be representable with an int
  // PropertyKey.
  static PropertyKey NonIntAtom(JSAtom* atom) {
    MOZ_ASSERT((uintptr_t(atom) & TypeMask) == 0);
    MOZ_ASSERT(PropertyKey::isNonIntAtom(atom));
    return PropertyKey::fromRawBits(uintptr_t(atom) | StringTypeTag);
  }

  // The JSAtom/JSString type exposed to embedders is opaque.
  static PropertyKey NonIntAtom(JSString* str) {
    MOZ_ASSERT((uintptr_t(str) & TypeMask) == 0);
    MOZ_ASSERT(PropertyKey::isNonIntAtom(str));
    return PropertyKey::fromRawBits(uintptr_t(str) | StringTypeTag);
  }

  // This API can be used by embedders to convert pinned (aka interned) strings,
  // as created by JS_AtomizeAndPinString, into PropertyKeys. This means the
  // string does not have to be explicitly rooted.
  //
  // Only use this API when absolutely necessary, otherwise use JS_StringToId.
  static PropertyKey fromPinnedString(JSString* str);

  // Internal API!
  // All string PropertyKeys are actually atomized.
  MOZ_ALWAYS_INLINE bool isAtom() const { return isString(); }

  MOZ_ALWAYS_INLINE bool isAtom(JSAtom* atom) const {
    MOZ_ASSERT(PropertyKey::isNonIntAtom(atom));
    return *this == NonIntAtom(atom);
  }

  MOZ_ALWAYS_INLINE JSAtom* toAtom() const {
    return reinterpret_cast<JSAtom*>(toString());
  }
  MOZ_ALWAYS_INLINE JSLinearString* toLinearString() const {
    return reinterpret_cast<JSLinearString*>(toString());
  }

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dump() const;
  void dump(js::GenericPrinter& out) const;
  void dump(js::JSONPrinter& json) const;

  void dumpFields(js::JSONPrinter& json) const;
  void dumpPropertyName(js::GenericPrinter& out) const;
  void dumpStringContent(js::GenericPrinter& out) const;
#endif

 private:
  static bool isNonIntAtom(JSAtom* atom);
  static bool isNonIntAtom(JSString* atom);
} JS_HAZ_GC_POINTER;

}  // namespace JS

using jsid = JS::PropertyKey;

namespace JS {

// Handle<PropertyKey> version of PropertyKey::Void().
extern JS_PUBLIC_DATA const JS::HandleId VoidHandlePropertyKey;

template <>
struct GCPolicy<jsid> {
  static void trace(JSTracer* trc, jsid* idp, const char* name) {
    // This should only be called as part of root marking since that's the only
    // time we should trace unbarriered GC thing pointers. This will assert if
    // called at other times.
    TraceRoot(trc, idp, name);
  }
  static bool isValid(jsid id) {
    return !id.isGCThing() ||
           js::gc::IsCellPointerValid(id.toGCCellPtr().asCell());
  }

  static bool isTenured(jsid id) {
    MOZ_ASSERT_IF(id.isGCThing(),
                  !js::gc::IsInsideNursery(id.toGCCellPtr().asCell()));
    return true;
  }
};

#ifdef DEBUG
MOZ_ALWAYS_INLINE void AssertIdIsNotGray(jsid id) {
  if (id.isGCThing()) {
    AssertCellIsNotGray(id.toGCCellPtr().asCell());
  }
}
#endif

/**
 * Get one of the well-known symbols defined by ES6 as PropertyKey. This is
 * equivalent to calling JS::GetWellKnownSymbol and then creating a PropertyKey.
 *
 * `which` must be in the range [0, WellKnownSymbolLimit).
 */
extern JS_PUBLIC_API PropertyKey GetWellKnownSymbolKey(JSContext* cx,
                                                       SymbolCode which);

/**
 * Generate getter/setter id for given id, by adding "get " or "set " prefix.
 */
extern JS_PUBLIC_API bool ToGetterId(
    JSContext* cx, JS::Handle<JS::PropertyKey> id,
    JS::MutableHandle<JS::PropertyKey> getterId);
extern JS_PUBLIC_API bool ToSetterId(
    JSContext* cx, JS::Handle<JS::PropertyKey> id,
    JS::MutableHandle<JS::PropertyKey> setterId);

}  // namespace JS

namespace js {

template <>
struct BarrierMethods<jsid> {
  static gc::Cell* asGCThingOrNull(jsid id) {
    if (id.isGCThing()) {
      return id.toGCThing();
    }
    return nullptr;
  }
  static void writeBarriers(jsid* idp, jsid prev, jsid next) {
    if (prev.isString()) {
      JS::IncrementalPreWriteBarrier(JS::GCCellPtr(prev.toString()));
    }
    if (prev.isSymbol()) {
      JS::IncrementalPreWriteBarrier(JS::GCCellPtr(prev.toSymbol()));
    }
    postWriteBarrier(idp, prev, next);
  }
  static void postWriteBarrier(jsid* idp, jsid prev, jsid next) {
    MOZ_ASSERT_IF(next.isString(), !gc::IsInsideNursery(next.toString()));
  }
  static void exposeToJS(jsid id) {
    if (id.isGCThing()) {
      js::gc::ExposeGCThingToActiveJS(id.toGCCellPtr());
    }
  }
  static void readBarrier(jsid id) {
    if (id.isGCThing()) {
      js::gc::IncrementalReadBarrier(id.toGCCellPtr());
    }
  }
};

// If the jsid is a GC pointer type, convert to that type and call |f| with the
// pointer and return the result wrapped in a Maybe, otherwise return None().
template <typename F>
auto MapGCThingTyped(const jsid& id, F&& f) {
  if (id.isString()) {
    return mozilla::Some(f(id.toString()));
  }
  if (id.isSymbol()) {
    return mozilla::Some(f(id.toSymbol()));
  }
  MOZ_ASSERT(!id.isGCThing());
  using ReturnType = decltype(f(static_cast<JSString*>(nullptr)));
  return mozilla::Maybe<ReturnType>();
}

// If the jsid is a GC pointer type, convert to that type and call |f| with the
// pointer. Return whether this happened.
template <typename F>
bool ApplyGCThingTyped(const jsid& id, F&& f) {
  return MapGCThingTyped(id,
                         [&f](auto t) {
                           f(t);
                           return true;
                         })
      .isSome();
}

template <typename Wrapper>
class WrappedPtrOperations<JS::PropertyKey, Wrapper> {
  const JS::PropertyKey& id() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  bool isVoid() const { return id().isVoid(); }
  bool isInt() const { return id().isInt(); }
  bool isString() const { return id().isString(); }
  bool isSymbol() const { return id().isSymbol(); }
  bool isGCThing() const { return id().isGCThing(); }

  int32_t toInt() const { return id().toInt(); }
  JSString* toString() const { return id().toString(); }
  JS::Symbol* toSymbol() const { return id().toSymbol(); }

  bool isPrivateName() const { return id().isPrivateName(); }

  bool isWellKnownSymbol(JS::SymbolCode code) const {
    return id().isWellKnownSymbol(code);
  }

  uintptr_t asRawBits() const { return id().asRawBits(); }

  // Internal API
  bool isAtom() const { return id().isAtom(); }
  bool isAtom(JSAtom* atom) const { return id().isAtom(atom); }
  JSAtom* toAtom() const { return id().toAtom(); }
  JSLinearString* toLinearString() const { return id().toLinearString(); }
};

}  // namespace js

#endif /* js_Id_h */
