/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Id_h
#define js_Id_h

// [SMDOC] Property Key / JSID
//
// A jsid is an identifier for a property or method of an object which is
// either a 31-bit unsigned integer, interned string or symbol.
//
// Also, there is an additional jsid value, JSID_VOID, which does not occur in
// JS scripts but may be used to indicate the absence of a valid jsid.  A void
// jsid is not a valid id and only arises as an exceptional API return value,
// such as in JS_NextProperty. Embeddings must not pass JSID_VOID into JSAPI
// entry points expecting a jsid and do not need to handle JSID_VOID in hooks
// receiving a jsid except when explicitly noted in the API contract.
//
// A jsid is not implicitly convertible to or from a Value; JS_ValueToId or
// JS_IdToValue must be used instead.

#include "mozilla/Maybe.h"

#include "jstypes.h"

#include "js/GCAnnotations.h"
#include "js/HeapAPI.h"
#include "js/RootingAPI.h"
#include "js/TraceKind.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"

// All jsids with the low bit set are integer ids. This means the other type
// tags must all be even.
#define JSID_TYPE_INT_BIT 0x1

// Use 0 for JSID_TYPE_STRING to avoid a bitwise op for atom <-> id conversions.
#define JSID_TYPE_STRING 0x0
#define JSID_TYPE_VOID 0x2
#define JSID_TYPE_SYMBOL 0x4
// (0x6 is unused)
#define JSID_TYPE_MASK 0x7

namespace JS {

enum class SymbolCode : uint32_t;

struct PropertyKey {
  size_t asBits;

  constexpr PropertyKey() : asBits(JSID_TYPE_VOID) {}

  static constexpr MOZ_ALWAYS_INLINE PropertyKey fromRawBits(size_t bits) {
    PropertyKey id;
    id.asBits = bits;
    return id;
  }

  bool operator==(const PropertyKey& rhs) const { return asBits == rhs.asBits; }
  bool operator!=(const PropertyKey& rhs) const { return asBits != rhs.asBits; }

  MOZ_ALWAYS_INLINE bool isVoid() const {
    MOZ_ASSERT_IF((asBits & JSID_TYPE_MASK) == JSID_TYPE_VOID,
                  asBits == JSID_TYPE_VOID);
    return asBits == JSID_TYPE_VOID;
  }

  MOZ_ALWAYS_INLINE bool isInt() const {
    return !!(asBits & JSID_TYPE_INT_BIT);
  }

  MOZ_ALWAYS_INLINE bool isString() const {
    return (asBits & JSID_TYPE_MASK) == JSID_TYPE_STRING;
  }

  MOZ_ALWAYS_INLINE bool isSymbol() const {
    return (asBits & JSID_TYPE_MASK) == JSID_TYPE_SYMBOL;
  }

  MOZ_ALWAYS_INLINE bool isGCThing() const { return isString() || isSymbol(); }

  MOZ_ALWAYS_INLINE int32_t toInt() const {
    MOZ_ASSERT(isInt());
    uint32_t bits = static_cast<uint32_t>(asBits) >> 1;
    return static_cast<int32_t>(bits);
  }

  MOZ_ALWAYS_INLINE JSString* toString() const {
    MOZ_ASSERT(isString());
    // Use XOR instead of `& ~JSID_TYPE_MASK` because small immediates can be
    // encoded more efficiently on some platorms.
    return reinterpret_cast<JSString*>(asBits ^ JSID_TYPE_STRING);
  }

  MOZ_ALWAYS_INLINE JS::Symbol* toSymbol() const {
    MOZ_ASSERT(isSymbol());
    return reinterpret_cast<JS::Symbol*>(asBits ^ JSID_TYPE_SYMBOL);
  }

  js::gc::Cell* toGCThing() const {
    MOZ_ASSERT(isGCThing());
    return reinterpret_cast<js::gc::Cell*>(asBits & ~(size_t)JSID_TYPE_MASK);
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

  // This API can be used by embedders to convert pinned (aka interned) strings,
  // as created by JS_AtomizeAndPinJSString, into PropertyKeys.
  // This means the string does not have to be explicitly rooted.
  //
  // Only use this API when absolutely necessary, otherwise use JS_StringToId.
  static PropertyKey fromPinnedString(JSString* str);

  // Must not be used on atoms that are representable as integer PropertyKey.
  // Prefer NameToId or AtomToId over this function:
  //
  // A PropertyName is an atom that does not contain an integer in the range
  // [0, UINT32_MAX]. However, PropertyKey can only hold an integer in the range
  // [0, JSID_INT_MAX] (where JSID_INT_MAX == 2^31-1).  Thus, for the range of
  // integers (JSID_INT_MAX, UINT32_MAX], to represent as a 'id', it must be
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
  static PropertyKey fromNonIntAtom(JSAtom* atom) {
    MOZ_ASSERT((size_t(atom) & JSID_TYPE_MASK) == 0);
    MOZ_ASSERT(PropertyKey::isNonIntAtom(atom));
    return PropertyKey::fromRawBits(size_t(atom) | JSID_TYPE_STRING);
  }

  // The JSAtom/JSString type exposed to embedders is opaque.
  static PropertyKey fromNonIntAtom(JSString* str) {
    MOZ_ASSERT((size_t(str) & JSID_TYPE_MASK) == 0);
    MOZ_ASSERT(PropertyKey::isNonIntAtom(str));
    return PropertyKey::fromRawBits(size_t(str) | JSID_TYPE_STRING);
  }

  // Internal API!
  // All string PropertyKeys are actually atomized.
  MOZ_ALWAYS_INLINE bool isAtom() const { return isString(); }

  MOZ_ALWAYS_INLINE bool isAtom(JSAtom* atom) const {
    MOZ_ASSERT(PropertyKey::isNonIntAtom(atom));
    return isAtom() && toAtom() == atom;
  }

  MOZ_ALWAYS_INLINE JSAtom* toAtom() const { return (JSAtom*)toString(); }

 private:
  static bool isNonIntAtom(JSAtom* atom);
  static bool isNonIntAtom(JSString* atom);
} JS_HAZ_GC_POINTER;

}  // namespace JS

using jsid = JS::PropertyKey;

#define JSID_BITS(id) (id.asBits)

static MOZ_ALWAYS_INLINE bool JSID_IS_STRING(jsid id) { return id.isString(); }

static MOZ_ALWAYS_INLINE JSString* JSID_TO_STRING(jsid id) {
  return id.toString();
}

static MOZ_ALWAYS_INLINE bool JSID_IS_INT(jsid id) { return id.isInt(); }

static MOZ_ALWAYS_INLINE int32_t JSID_TO_INT(jsid id) { return id.toInt(); }

#define JSID_INT_MIN 0
#define JSID_INT_MAX INT32_MAX

static MOZ_ALWAYS_INLINE bool INT_FITS_IN_JSID(int32_t i) { return i >= 0; }

static MOZ_ALWAYS_INLINE jsid INT_TO_JSID(int32_t i) {
  jsid id;
  MOZ_ASSERT(INT_FITS_IN_JSID(i));
  uint32_t bits = (static_cast<uint32_t>(i) << 1) | JSID_TYPE_INT_BIT;
  JSID_BITS(id) = static_cast<size_t>(bits);
  return id;
}

static MOZ_ALWAYS_INLINE jsid SYMBOL_TO_JSID(JS::Symbol* sym) {
  jsid id;
  MOZ_ASSERT(sym != nullptr);
  MOZ_ASSERT((size_t(sym) & JSID_TYPE_MASK) == 0);
  MOZ_ASSERT(!js::gc::IsInsideNursery(reinterpret_cast<js::gc::Cell*>(sym)));
  JSID_BITS(id) = (size_t(sym) | JSID_TYPE_SYMBOL);
  return id;
}

static MOZ_ALWAYS_INLINE bool JSID_IS_VOID(const jsid id) {
  return id.isVoid();
}

constexpr const jsid JSID_VOID;

extern JS_PUBLIC_DATA const JS::HandleId JSID_VOIDHANDLE;

namespace JS {

template <>
struct GCPolicy<jsid> {
  static void trace(JSTracer* trc, jsid* idp, const char* name) {
    // It's not safe to trace unbarriered pointers except as part of root
    // marking.
    UnsafeTraceRoot(trc, idp, name);
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
  static void postWriteBarrier(jsid* idp, jsid prev, jsid next) {
    MOZ_ASSERT_IF(JSID_IS_STRING(next),
                  !gc::IsInsideNursery(JSID_TO_STRING(next)));
  }
  static void exposeToJS(jsid id) {
    if (id.isGCThing()) {
      js::gc::ExposeGCThingToActiveJS(id.toGCCellPtr());
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

  // Internal API
  bool isAtom() const { return id().isAtom(); }
  bool isAtom(JSAtom* atom) const { return id().isAtom(atom); }
  JSAtom* toAtom() const { return id().toAtom(); }
};

}  // namespace js

#endif /* js_Id_h */
