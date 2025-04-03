/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_experimental_JitInfo_h
#define js_experimental_JitInfo_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stddef.h>  // size_t
#include <stdint.h>  // uint16_t, uint32_t

#include "js/CallArgs.h"    // JS::CallArgs, JS::detail::CallArgsBase, JSNative
#include "js/RootingAPI.h"  // JS::{,Mutable}Handle, JS::Rooted
#include "js/Value.h"       // JS::Value, JSValueType

namespace js {

namespace jit {

enum class InlinableNative : uint16_t;

}  // namespace jit

}  // namespace js

/**
 * A class, expected to be passed by value, which represents the CallArgs for a
 * JSJitGetterOp.
 */
class JSJitGetterCallArgs : protected JS::MutableHandle<JS::Value> {
 public:
  explicit JSJitGetterCallArgs(const JS::CallArgs& args)
      : JS::MutableHandle<JS::Value>(args.rval()) {}

  explicit JSJitGetterCallArgs(JS::Rooted<JS::Value>* rooted)
      : JS::MutableHandle<JS::Value>(rooted) {}

  explicit JSJitGetterCallArgs(JS::MutableHandle<JS::Value> handle)
      : JS::MutableHandle<JS::Value>(handle) {}

  JS::MutableHandle<JS::Value> rval() { return *this; }
};

/**
 * A class, expected to be passed by value, which represents the CallArgs for a
 * JSJitSetterOp.
 */
class JSJitSetterCallArgs : protected JS::MutableHandle<JS::Value> {
 public:
  explicit JSJitSetterCallArgs(const JS::CallArgs& args)
      : JS::MutableHandle<JS::Value>(args[0]) {}

  explicit JSJitSetterCallArgs(JS::Rooted<JS::Value>* rooted)
      : JS::MutableHandle<JS::Value>(rooted) {}

  JS::MutableHandle<JS::Value> operator[](unsigned i) {
    MOZ_ASSERT(i == 0);
    return *this;
  }

  unsigned length() const { return 1; }

  // Add get() or maybe hasDefined() as needed
};

struct JSJitMethodCallArgsTraits;

/**
 * A class, expected to be passed by reference, which represents the CallArgs
 * for a JSJitMethodOp.
 */
class JSJitMethodCallArgs
    : protected JS::detail::CallArgsBase<JS::detail::NoUsedRval> {
 private:
  using Base = JS::detail::CallArgsBase<JS::detail::NoUsedRval>;
  friend struct JSJitMethodCallArgsTraits;

 public:
  explicit JSJitMethodCallArgs(const JS::CallArgs& args) {
    argv_ = args.array();
    argc_ = args.length();
  }

  JS::MutableHandle<JS::Value> rval() const { return Base::rval(); }

  unsigned length() const { return Base::length(); }

  JS::MutableHandle<JS::Value> operator[](unsigned i) const {
    return Base::operator[](i);
  }

  bool hasDefined(unsigned i) const { return Base::hasDefined(i); }

  JSObject& callee() const {
    // We can't use Base::callee() because that will try to poke at
    // this->usedRval_, which we don't have.
    return argv_[-2].toObject();
  }

  JS::Handle<JS::Value> get(unsigned i) const { return Base::get(i); }

  bool requireAtLeast(JSContext* cx, const char* fnname,
                      unsigned required) const {
    // Can just forward to Base, since it only needs the length and we
    // forward that already.
    return Base::requireAtLeast(cx, fnname, required);
  }
};

struct JSJitMethodCallArgsTraits {
  static constexpr size_t offsetOfArgv = offsetof(JSJitMethodCallArgs, argv_);
  static constexpr size_t offsetOfArgc = offsetof(JSJitMethodCallArgs, argc_);
};

using JSJitGetterOp = bool (*)(JSContext*, JS::Handle<JSObject*>, void*,
                               JSJitGetterCallArgs);
using JSJitSetterOp = bool (*)(JSContext*, JS::Handle<JSObject*>, void*,
                               JSJitSetterCallArgs);
using JSJitMethodOp = bool (*)(JSContext*, JS::Handle<JSObject*>, void*,
                               const JSJitMethodCallArgs&);

/**
 * This struct contains metadata passed from the DOM to the JS Engine for JIT
 * optimizations on DOM property accessors.
 *
 * Eventually, this should be made available to general JSAPI users as *not*
 * experimental and *not* a friend API, but we're not ready to do so yet.
 */
class JSJitInfo {
 public:
  enum OpType {
    Getter,
    Setter,
    Method,
    StaticMethod,
    InlinableNative,
    IgnoresReturnValueNative,
    // Must be last
    OpTypeCount
  };

  enum ArgType {
    // Basic types
    String = (1 << 0),
    Integer = (1 << 1),  // Only 32-bit or less
    Double = (1 << 2),   // Maybe we want to add Float sometime too
    Boolean = (1 << 3),
    Object = (1 << 4),
    Null = (1 << 5),

    // And derived types
    Numeric = Integer | Double,
    // Should "Primitive" use the WebIDL definition, which
    // excludes string and null, or the typical JS one that includes them?
    Primitive = Numeric | Boolean | Null | String,
    ObjectOrNull = Object | Null,
    Any = ObjectOrNull | Primitive,

    // Our sentinel value.
    ArgTypeListEnd = (1 << 31)
  };

  static_assert(Any & String, "Any must include String");
  static_assert(Any & Integer, "Any must include Integer");
  static_assert(Any & Double, "Any must include Double");
  static_assert(Any & Boolean, "Any must include Boolean");
  static_assert(Any & Object, "Any must include Object");
  static_assert(Any & Null, "Any must include Null");

  /**
   * An enum that describes what this getter/setter/method aliases.  This
   * determines what things can be hoisted past this call, and if this
   * call is movable what it can be hoisted past.
   */
  enum AliasSet {
    /**
     * Alias nothing: a constant value, getting it can't affect any other
     * values, nothing can affect it.
     */
    AliasNone,

    /**
     * Alias things that can modify the DOM but nothing else.  Doing the
     * call can't affect the behavior of any other function.
     */
    AliasDOMSets,

    /**
     * Alias the world.  Calling this can change arbitrary values anywhere
     * in the system.  Most things fall in this bucket.
     */
    AliasEverything,

    /** Must be last. */
    AliasSetCount
  };

  bool needsOuterizedThisObject() const {
    return type() != Getter && type() != Setter;
  }

  bool isTypedMethodJitInfo() const { return isTypedMethod; }

  OpType type() const { return OpType(type_); }

  AliasSet aliasSet() const { return AliasSet(aliasSet_); }

  JSValueType returnType() const { return JSValueType(returnType_); }

  union {
    JSJitGetterOp getter;
    JSJitSetterOp setter;
    JSJitMethodOp method;
    /** A DOM static method, used for Promise wrappers */
    JSNative staticMethod;
    JSNative ignoresReturnValueMethod;
  };

  static unsigned offsetOfIgnoresReturnValueNative() {
    return offsetof(JSJitInfo, ignoresReturnValueMethod);
  }

  union {
    uint16_t protoID;
    js::jit::InlinableNative inlinableNative;
  };

  union {
    uint16_t depth;

    // Additional opcode for some InlinableNative functions.
    uint16_t nativeOp;
  };

  // These fields are carefully packed to take up 4 bytes.  If you need more
  // bits for whatever reason, please see if you can steal bits from existing
  // fields before adding more members to this structure.
  static constexpr size_t OpTypeBits = 4;
  static constexpr size_t AliasSetBits = 4;
  static constexpr size_t ReturnTypeBits = 8;
  static constexpr size_t SlotIndexBits = 10;

  /** The OpType that says what sort of function we are. */
  uint32_t type_ : OpTypeBits;

  /**
   * The alias set for this op.  This is a _minimal_ alias set; in
   * particular for a method it does not include whatever argument
   * conversions might do.  That's covered by argTypes and runtime
   * analysis of the actual argument types being passed in.
   */
  uint32_t aliasSet_ : AliasSetBits;

  /** The return type tag.  Might be JSVAL_TYPE_UNKNOWN. */
  uint32_t returnType_ : ReturnTypeBits;

  static_assert(OpTypeCount <= (1 << OpTypeBits),
                "Not enough space for OpType");
  static_assert(AliasSetCount <= (1 << AliasSetBits),
                "Not enough space for AliasSet");
  static_assert((sizeof(JSValueType) * 8) <= ReturnTypeBits,
                "Not enough space for JSValueType");

  /** Is op fallible? False in setters. */
  uint32_t isInfallible : 1;

  /**
   * Is op movable?  To be movable the op must
   * not AliasEverything, but even that might
   * not be enough (e.g. in cases when it can
   * throw or is explicitly not movable).
   */
  uint32_t isMovable : 1;

  /**
   * Can op be dead-code eliminated? Again, this
   * depends on whether the op can throw, in
   * addition to the alias set.
   */
  uint32_t isEliminatable : 1;

  // XXXbz should we have a JSValueType for the type of the member?
  /**
   * True if this is a getter that can always
   * get the value from a slot of the "this" object.
   */
  uint32_t isAlwaysInSlot : 1;

  /**
   * True if this is a getter that can sometimes (if the slot doesn't contain
   * UndefinedValue()) get the value from a slot of the "this" object.
   */
  uint32_t isLazilyCachedInSlot : 1;

  /** True if this is an instance of JSTypedMethodJitInfo. */
  uint32_t isTypedMethod : 1;

  /**
   * If isAlwaysInSlot or isSometimesInSlot is true,
   * the index of the slot to get the value from.
   * Otherwise 0.
   */
  uint32_t slotIndex : SlotIndexBits;

  static constexpr size_t maxSlotIndex = (1 << SlotIndexBits) - 1;
};

static_assert(sizeof(JSJitInfo) == (sizeof(void*) + 2 * sizeof(uint32_t)),
              "There are several thousand instances of JSJitInfo stored in "
              "a binary. Please don't increase its space requirements without "
              "verifying that there is no other way forward (better packing, "
              "smaller datatypes for fields, subclassing, etc.).");

struct JSTypedMethodJitInfo {
  // We use C-style inheritance here, rather than C++ style inheritance
  // because not all compilers support brace-initialization for non-aggregate
  // classes. Using C++ style inheritance and constructors instead of
  // brace-initialization would also force the creation of static
  // constructors (on some compilers) when JSJitInfo and JSTypedMethodJitInfo
  // structures are declared. Since there can be several thousand of these
  // structures present and we want to have roughly equivalent performance
  // across a range of compilers, we do things manually.
  JSJitInfo base;

  const JSJitInfo::ArgType* const argTypes; /* For a method, a list of sets of
                                               types that the function
                                               expects.  This can be used,
                                               for example, to figure out
                                               when argument coercions can
                                               have side-effects. */
};

#endif  // js_experimental_JitInfo_h
