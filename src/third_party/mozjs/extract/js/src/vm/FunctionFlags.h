/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_FunctionFlags_h
#define vm_FunctionFlags_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_ASSERT_IF
#include "mozilla/Attributes.h"  // MOZ_IMPLICIT

#include <stdint.h>  // uint8_t, uint16_t

#include "jstypes.h"  // JS_PUBLIC_API

class JS_PUBLIC_API JSAtom;

namespace js {

class FunctionFlags {
 public:
  enum FunctionKind : uint8_t {
    NormalFunction = 0,
    Arrow,   // ES6 '(args) => body' syntax
    Method,  // ES6 MethodDefinition
    ClassConstructor,
    Getter,
    Setter,
    AsmJS,  // An asm.js module or exported function
    Wasm,   // An exported WebAssembly function
    FunctionKindLimit
  };

  enum Flags : uint16_t {
    // The general kind of a function. This is used to describe characteristics
    // of functions that do not merit a dedicated flag bit below.
    FUNCTION_KIND_SHIFT = 0,
    FUNCTION_KIND_MASK = 0x0007,

    // The AllocKind used was FunctionExtended and extra slots were allocated.
    // These slots may be used by the engine or the embedding so care must be
    // taken to avoid conflicts.
    EXTENDED = 1 << 3,

    // Set if function is a self-hosted builtin or intrinsic. An 'intrinsic'
    // here means a native function used inside self-hosted code. In general, a
    // self-hosted function should appear to script as though it were a native
    // builtin.
    SELF_HOSTED = 1 << 4,

    // An interpreted function has or may-have bytecode and an environment. Only
    // one of these flags may be used at a time. As a memory optimization, the
    // SELFHOSTLAZY flag indicates there is no js::BaseScript at all and we must
    // clone from the self-hosted realm in order to get bytecode.
    BASESCRIPT = 1 << 5,
    SELFHOSTLAZY = 1 << 6,

    // Function may be called as a constructor. This corresponds in the spec as
    // having a [[Construct]] internal method.
    CONSTRUCTOR = 1 << 7,

    // A 'Bound Function Exotic Object' created by Function.prototype.bind.
    BOUND_FUN = 1 << 8,

    // Function comes from a FunctionExpression, ArrowFunction, or Function()
    // call (not a FunctionDeclaration or nonstandard function-statement).
    LAMBDA = 1 << 9,

    // The WASM function has a JIT entry which emulates the
    // js::BaseScript::jitCodeRaw mechanism.
    WASM_JIT_ENTRY = 1 << 10,

    // Function had no explicit name, but a name was set by SetFunctionName at
    // compile time or SetFunctionName at runtime.
    HAS_INFERRED_NAME = 1 << 11,

    // Function had no explicit name, but a name was guessed for it anyway. For
    // a Bound function, tracks if atom_ already contains the "bound " prefix.
    ATOM_EXTRA_FLAG = 1 << 12,
    HAS_GUESSED_ATOM = ATOM_EXTRA_FLAG,
    HAS_BOUND_FUNCTION_NAME_PREFIX = ATOM_EXTRA_FLAG,

    // The 'length' or 'name property has been resolved. See fun_resolve.
    RESOLVED_NAME = 1 << 13,
    RESOLVED_LENGTH = 1 << 14,

    // This function is kept only for skipping it over during delazification.
    //
    // This function is inside arrow function's parameter expression, and
    // parsed twice, once before finding "=>" token, and once after finding
    // "=>" and rewinding to the beginning of the parameters.
    // ScriptStencil is created for both case, and the first one is kept only
    // for delazification, to make sure delazification sees the same sequence
    // of inner function to skip over.
    //
    // We call the first one "ghost".
    // It should be kept lazy, and shouldn't be exposed to debugger.
    GHOST_FUNCTION = 1 << 15,

    // Shifted form of FunctionKinds.
    NORMAL_KIND = NormalFunction << FUNCTION_KIND_SHIFT,
    ASMJS_KIND = AsmJS << FUNCTION_KIND_SHIFT,
    WASM_KIND = Wasm << FUNCTION_KIND_SHIFT,
    ARROW_KIND = Arrow << FUNCTION_KIND_SHIFT,
    METHOD_KIND = Method << FUNCTION_KIND_SHIFT,
    CLASSCONSTRUCTOR_KIND = ClassConstructor << FUNCTION_KIND_SHIFT,
    GETTER_KIND = Getter << FUNCTION_KIND_SHIFT,
    SETTER_KIND = Setter << FUNCTION_KIND_SHIFT,

    // Derived Flags combinations to use when creating functions.
    NATIVE_FUN = NORMAL_KIND,
    NATIVE_CTOR = CONSTRUCTOR | NORMAL_KIND,
    ASMJS_CTOR = CONSTRUCTOR | ASMJS_KIND,
    ASMJS_LAMBDA_CTOR = CONSTRUCTOR | LAMBDA | ASMJS_KIND,
    WASM = WASM_KIND,
    INTERPRETED_NORMAL = BASESCRIPT | CONSTRUCTOR | NORMAL_KIND,
    INTERPRETED_CLASS_CTOR = BASESCRIPT | CONSTRUCTOR | CLASSCONSTRUCTOR_KIND,
    INTERPRETED_GENERATOR_OR_ASYNC = BASESCRIPT | NORMAL_KIND,
    INTERPRETED_LAMBDA = BASESCRIPT | LAMBDA | CONSTRUCTOR | NORMAL_KIND,
    INTERPRETED_LAMBDA_ARROW = BASESCRIPT | LAMBDA | ARROW_KIND,
    INTERPRETED_LAMBDA_GENERATOR_OR_ASYNC = BASESCRIPT | LAMBDA | NORMAL_KIND,
    INTERPRETED_GETTER = BASESCRIPT | GETTER_KIND,
    INTERPRETED_SETTER = BASESCRIPT | SETTER_KIND,
    INTERPRETED_METHOD = BASESCRIPT | METHOD_KIND,

    // Flags that XDR ignores. See also: js::BaseScript::MutableFlags.
    MUTABLE_FLAGS = RESOLVED_NAME | RESOLVED_LENGTH,

    // Flags preserved when cloning a function.
    STABLE_ACROSS_CLONES =
        CONSTRUCTOR | LAMBDA | SELF_HOSTED | FUNCTION_KIND_MASK | GHOST_FUNCTION
  };

  uint16_t flags_;

 public:
  FunctionFlags() : flags_() {
    static_assert(sizeof(FunctionFlags) == sizeof(flags_),
                  "No extra members allowed is it'll grow JSFunction");
    static_assert(offsetof(FunctionFlags, flags_) == 0,
                  "Required for JIT flag access");
  }

  explicit FunctionFlags(uint16_t flags) : flags_(flags) {}
  MOZ_IMPLICIT FunctionFlags(Flags f) : flags_(f) {}

  static_assert(((FunctionKindLimit - 1) << FUNCTION_KIND_SHIFT) <=
                    FUNCTION_KIND_MASK,
                "FunctionKind doesn't fit into flags_");

  uint16_t toRaw() const { return flags_; }

  uint16_t stableAcrossClones() const { return flags_ & STABLE_ACROSS_CLONES; }

  // For flag combinations the type is int.
  bool hasFlags(uint16_t flags) const { return flags_ & flags; }
  void setFlags(uint16_t flags) { flags_ |= flags; }
  void clearFlags(uint16_t flags) { flags_ &= ~flags; }
  void setFlags(uint16_t flags, bool set) {
    if (set) {
      setFlags(flags);
    } else {
      clearFlags(flags);
    }
  }

  FunctionKind kind() const {
    return static_cast<FunctionKind>((flags_ & FUNCTION_KIND_MASK) >>
                                     FUNCTION_KIND_SHIFT);
  }

  /* A function can be classified as either native (C++) or interpreted (JS): */
  bool isInterpreted() const {
    return hasFlags(BASESCRIPT) || hasFlags(SELFHOSTLAZY);
  }
  bool isNativeFun() const { return !isInterpreted(); }

  bool isConstructor() const { return hasFlags(CONSTRUCTOR); }

  bool isNonBuiltinConstructor() const {
    // Note: keep this in sync with branchIfNotFunctionIsNonBuiltinCtor in
    // MacroAssembler.cpp.
    return hasFlags(BASESCRIPT) && hasFlags(CONSTRUCTOR) &&
           !hasFlags(SELF_HOSTED);
  }

  /* Possible attributes of a native function: */
  bool isAsmJSNative() const {
    MOZ_ASSERT_IF(kind() == AsmJS, isNativeFun());
    return kind() == AsmJS;
  }
  bool isWasm() const {
    MOZ_ASSERT_IF(kind() == Wasm, isNativeFun());
    return kind() == Wasm;
  }
  bool isWasmWithJitEntry() const {
    MOZ_ASSERT_IF(hasFlags(WASM_JIT_ENTRY), isWasm());
    return hasFlags(WASM_JIT_ENTRY);
  }
  bool isNativeWithoutJitEntry() const {
    MOZ_ASSERT_IF(!hasJitEntry(), isNativeFun());
    return !hasJitEntry();
  }
  bool isBuiltinNative() const {
    return isNativeFun() && !isAsmJSNative() && !isWasm();
  }
  bool hasJitEntry() const {
    return hasBaseScript() || hasSelfHostedLazyScript() || isWasmWithJitEntry();
  }

  /* Possible attributes of an interpreted function: */
  bool isBoundFunction() const { return hasFlags(BOUND_FUN); }
  bool hasInferredName() const { return hasFlags(HAS_INFERRED_NAME); }
  bool hasGuessedAtom() const {
    static_assert(HAS_GUESSED_ATOM == HAS_BOUND_FUNCTION_NAME_PREFIX,
                  "HAS_GUESSED_ATOM is unused for bound functions");
    bool hasGuessedAtom = hasFlags(HAS_GUESSED_ATOM);
    bool boundFun = hasFlags(BOUND_FUN);
    return hasGuessedAtom && !boundFun;
  }
  bool hasBoundFunctionNamePrefix() const {
    static_assert(
        HAS_BOUND_FUNCTION_NAME_PREFIX == HAS_GUESSED_ATOM,
        "HAS_BOUND_FUNCTION_NAME_PREFIX is only used for bound functions");
    MOZ_ASSERT(isBoundFunction());
    return hasFlags(HAS_BOUND_FUNCTION_NAME_PREFIX);
  }
  bool isLambda() const { return hasFlags(LAMBDA); }

  bool isNamedLambda(bool hasName) const {
    return hasName && isLambda() && !hasInferredName() && !hasGuessedAtom();
  }

  // These methods determine which of the u.scripted.s union arms are active.
  // For live JSFunctions the pointer values will always be non-null, but due
  // to partial initialization the GC (and other features that scan the heap
  // directly) may still return a null pointer.
  bool hasBaseScript() const { return hasFlags(BASESCRIPT); }
  bool hasSelfHostedLazyScript() const { return hasFlags(SELFHOSTLAZY); }

  // Arrow functions store their lexical new.target in the first extended slot.
  bool isArrow() const { return kind() == Arrow; }
  // Every class-constructor is also a method.
  bool isMethod() const {
    return kind() == Method || kind() == ClassConstructor;
  }
  bool isClassConstructor() const { return kind() == ClassConstructor; }

  bool isGetter() const { return kind() == Getter; }
  bool isSetter() const { return kind() == Setter; }

  bool allowSuperProperty() const {
    return isMethod() || isGetter() || isSetter();
  }

  bool hasResolvedLength() const { return hasFlags(RESOLVED_LENGTH); }
  bool hasResolvedName() const { return hasFlags(RESOLVED_NAME); }

  bool isSelfHostedOrIntrinsic() const { return hasFlags(SELF_HOSTED); }
  bool isSelfHostedBuiltin() const {
    return isSelfHostedOrIntrinsic() && !isNativeFun();
  }
  bool isIntrinsic() const {
    return isSelfHostedOrIntrinsic() && isNativeFun();
  }

  void setKind(FunctionKind kind) {
    this->flags_ &= ~FUNCTION_KIND_MASK;
    this->flags_ |= static_cast<uint16_t>(kind) << FUNCTION_KIND_SHIFT;
  }

  // Make the function constructible.
  void setIsConstructor() {
    MOZ_ASSERT(!isConstructor());
    MOZ_ASSERT(isSelfHostedBuiltin());
    setFlags(CONSTRUCTOR);
  }

  void setIsBoundFunction() {
    MOZ_ASSERT(!isBoundFunction());
    setFlags(BOUND_FUN);
  }

  void setIsSelfHostedBuiltin() {
    MOZ_ASSERT(isInterpreted());
    MOZ_ASSERT(!isSelfHostedBuiltin());
    setFlags(SELF_HOSTED);
    // Self-hosted functions should not be constructable.
    clearFlags(CONSTRUCTOR);
  }
  void setIsIntrinsic() {
    MOZ_ASSERT(isNativeFun());
    MOZ_ASSERT(!isIntrinsic());
    setFlags(SELF_HOSTED);
  }

  void setResolvedLength() { setFlags(RESOLVED_LENGTH); }
  void setResolvedName() { setFlags(RESOLVED_NAME); }

  void setInferredName() { setFlags(HAS_INFERRED_NAME); }

  void setGuessedAtom() { setFlags(HAS_GUESSED_ATOM); }

  void setPrefixedBoundFunctionName() {
    setFlags(HAS_BOUND_FUNCTION_NAME_PREFIX);
  }

  void setSelfHostedLazy() { setFlags(SELFHOSTLAZY); }
  void clearSelfHostedLazy() { clearFlags(SELFHOSTLAZY); }
  void setBaseScript() { setFlags(BASESCRIPT); }
  void clearBaseScript() { clearFlags(BASESCRIPT); }

  void setWasmJitEntry() { setFlags(WASM_JIT_ENTRY); }

  bool isExtended() const { return hasFlags(EXTENDED); }
  void setIsExtended() { setFlags(EXTENDED); }

  bool isNativeConstructor() const { return hasFlags(NATIVE_CTOR); }

  void setIsGhost() { setFlags(GHOST_FUNCTION); }
  bool isGhost() const { return hasFlags(GHOST_FUNCTION); }

  static uint16_t HasJitEntryFlags(bool isConstructing) {
    uint16_t flags = BASESCRIPT | SELFHOSTLAZY;
    if (!isConstructing) {
      flags |= WASM_JIT_ENTRY;
    }
    return flags;
  }

  static FunctionFlags clearMutableflags(FunctionFlags flags) {
    return FunctionFlags(flags.toRaw() & ~FunctionFlags::MUTABLE_FLAGS);
  }
};

} /* namespace js */

#endif /* vm_FunctionFlags_h */
