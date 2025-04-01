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
  // Syntactic characteristics of a function.
  enum FunctionKind : uint8_t {
    // Regular function that doesn't match any of the other kinds.
    //
    // This kind is used by the following scipted functions:
    //   * FunctionDeclaration
    //   * FunctionExpression
    //   * Function created from Function() call or its variants
    //
    // Also all native functions excluding AsmJS and Wasm use this kind.
    NormalFunction = 0,

    // ES6 '(args) => body' syntax.
    // This kind is used only by scripted function.
    Arrow,

    // ES6 MethodDefinition syntax.
    // This kind is used only by scripted function.
    Method,

    // Class constructor syntax, or default constructor.
    // This kind is used only by scripted function and default constructor.
    //
    // WARNING: This is independent from Flags::CONSTRUCTOR.
    ClassConstructor,

    // Getter and setter syntax in objects or classes, or
    // native getter and setter created from JSPropertySpec.
    // This kind is used both by scripted functions and native functions.
    Getter,
    Setter,

    // An asm.js module or exported function.
    //
    // This kind is used only by scripted function, and used only when the
    // asm.js module is created.
    //
    // "use asm" directive itself doesn't necessarily imply this kind.
    // e.g. arrow function with "use asm" becomes Arrow kind,
    //
    // See EstablishPreconditions in js/src/wasm/AsmJS.cpp
    AsmJS,

    // An exported WebAssembly function.
    Wasm,

    FunctionKindLimit
  };

  enum Flags : uint16_t {
    // FunctionKind enum value.
    FUNCTION_KIND_SHIFT = 0,
    FUNCTION_KIND_MASK = 0x0007,

    // The AllocKind used was FunctionExtended and extra slots were allocated.
    // These slots may be used by the engine or the embedding so care must be
    // taken to avoid conflicts.
    //
    // This flag is used both by scripted functions and native functions.
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

    // This Native function has a JIT entry which emulates the
    // js::BaseScript::jitCodeRaw mechanism. Used for Wasm functions and
    // TrampolineNative builtins.
    NATIVE_JIT_ENTRY = 1 << 7,

    // Function may be called as a constructor. This corresponds in the spec as
    // having a [[Construct]] internal method.
    //
    // e.g. FunctionDeclaration has this flag, but GeneratorDeclaration doesn't
    //      have this flag.
    //
    // This flag is used both by scripted functions and native functions.
    //
    // WARNING: This is independent from FunctionKind::ClassConstructor.
    CONSTRUCTOR = 1 << 8,

    // Function comes from a FunctionExpression, ArrowFunction, or Function()
    // call (not a FunctionDeclaration).
    //
    // This flag is used only by scripted functions and AsmJS.
    LAMBDA = 1 << 9,

    // Function is either getter or setter, with "get " or "set " prefix,
    // but JSFunction::AtomSlot contains unprefixed name, and the function name
    // is lazily constructed on the first access.
    LAZY_ACCESSOR_NAME = 1 << 10,

    // Function had no explicit name, but a name was set by SetFunctionName at
    // compile time or SetFunctionName at runtime.
    //
    // This flag can be used both by scripted functions and native functions.
    HAS_INFERRED_NAME = 1 << 11,

    // Function had no explicit name, but a name was guessed for it anyway.
    //
    // This flag is used only by scripted function.
    HAS_GUESSED_ATOM = 1 << 12,

    // The 'length' or 'name property has been resolved. See fun_resolve.
    //
    // These flags are used both by scripted functions and native functions.
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
    //
    // This flag is used only by scripted functions.
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
    NATIVE_GETTER_WITH_LAZY_NAME = LAZY_ACCESSOR_NAME | GETTER_KIND,
    NATIVE_SETTER_WITH_LAZY_NAME = LAZY_ACCESSOR_NAME | SETTER_KIND,
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
  FunctionFlags& setFlags(uint16_t flags) {
    flags_ |= flags;
    return *this;
  }
  FunctionFlags& clearFlags(uint16_t flags) {
    flags_ &= ~flags;
    return *this;
  }
  FunctionFlags& setFlags(uint16_t flags, bool set) {
    if (set) {
      setFlags(flags);
    } else {
      clearFlags(flags);
    }
    return *this;
  }

  FunctionKind kind() const {
    return static_cast<FunctionKind>((flags_ & FUNCTION_KIND_MASK) >>
                                     FUNCTION_KIND_SHIFT);
  }

#ifdef DEBUG
  void assertFunctionKindIntegrity() {
    switch (kind()) {
      case FunctionKind::NormalFunction:
        MOZ_ASSERT(!hasFlags(LAZY_ACCESSOR_NAME));
        MOZ_ASSERT(!hasFlags(NATIVE_JIT_ENTRY));
        break;

      case FunctionKind::Arrow:
        MOZ_ASSERT(hasFlags(BASESCRIPT) || hasFlags(SELFHOSTLAZY));
        MOZ_ASSERT(!hasFlags(CONSTRUCTOR));
        MOZ_ASSERT(!hasFlags(LAZY_ACCESSOR_NAME));
        MOZ_ASSERT(hasFlags(LAMBDA));
        MOZ_ASSERT(!hasFlags(NATIVE_JIT_ENTRY));
        break;
      case FunctionKind::Method:
        MOZ_ASSERT(hasFlags(BASESCRIPT) || hasFlags(SELFHOSTLAZY));
        MOZ_ASSERT(!hasFlags(CONSTRUCTOR));
        MOZ_ASSERT(!hasFlags(LAZY_ACCESSOR_NAME));
        MOZ_ASSERT(!hasFlags(LAMBDA));
        MOZ_ASSERT(!hasFlags(NATIVE_JIT_ENTRY));
        break;
      case FunctionKind::ClassConstructor:
        MOZ_ASSERT(hasFlags(BASESCRIPT) || hasFlags(SELFHOSTLAZY));
        MOZ_ASSERT(hasFlags(CONSTRUCTOR));
        MOZ_ASSERT(!hasFlags(LAZY_ACCESSOR_NAME));
        MOZ_ASSERT(!hasFlags(LAMBDA));
        MOZ_ASSERT(!hasFlags(NATIVE_JIT_ENTRY));
        break;
      case FunctionKind::Getter:
        MOZ_ASSERT(!hasFlags(CONSTRUCTOR));
        MOZ_ASSERT(!hasFlags(LAMBDA));
        MOZ_ASSERT(!hasFlags(NATIVE_JIT_ENTRY));
        break;
      case FunctionKind::Setter:
        MOZ_ASSERT(!hasFlags(CONSTRUCTOR));
        MOZ_ASSERT(!hasFlags(LAMBDA));
        MOZ_ASSERT(!hasFlags(NATIVE_JIT_ENTRY));
        break;

      case FunctionKind::AsmJS:
        MOZ_ASSERT(!hasFlags(BASESCRIPT));
        MOZ_ASSERT(!hasFlags(SELFHOSTLAZY));
        MOZ_ASSERT(!hasFlags(LAZY_ACCESSOR_NAME));
        MOZ_ASSERT(!hasFlags(NATIVE_JIT_ENTRY));
        break;
      case FunctionKind::Wasm:
        MOZ_ASSERT(!hasFlags(BASESCRIPT));
        MOZ_ASSERT(!hasFlags(SELFHOSTLAZY));
        MOZ_ASSERT(!hasFlags(CONSTRUCTOR));
        MOZ_ASSERT(!hasFlags(LAZY_ACCESSOR_NAME));
        MOZ_ASSERT(!hasFlags(LAMBDA));
        break;
      default:
        break;
    }
  }
#endif

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
  bool isNativeWithJitEntry() const {
    MOZ_ASSERT_IF(hasFlags(NATIVE_JIT_ENTRY), isNativeFun());
    return hasFlags(NATIVE_JIT_ENTRY);
  }
  bool isWasmWithJitEntry() const { return isWasm() && isNativeWithJitEntry(); }
  bool isNativeWithoutJitEntry() const {
    MOZ_ASSERT_IF(!hasJitEntry(), isNativeFun());
    return !hasJitEntry();
  }
  bool isBuiltinNative() const {
    return isNativeFun() && !isAsmJSNative() && !isWasm();
  }
  bool hasJitEntry() const {
    return hasBaseScript() || hasSelfHostedLazyScript() ||
           isNativeWithJitEntry();
  }

  bool canHaveJitInfo() const {
    // A native builtin can have a pointer to either its JitEntry or JSJitInfo,
    // but not both.
    return isBuiltinNative() && !isNativeWithJitEntry();
  }

  /* Possible attributes of an interpreted function: */
  bool hasInferredName() const { return hasFlags(HAS_INFERRED_NAME); }
  bool hasGuessedAtom() const { return hasFlags(HAS_GUESSED_ATOM); }
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

  bool isAccessorWithLazyName() const { return hasFlags(LAZY_ACCESSOR_NAME); }

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

  FunctionFlags& setKind(FunctionKind kind) {
    this->flags_ &= ~FUNCTION_KIND_MASK;
    this->flags_ |= static_cast<uint16_t>(kind) << FUNCTION_KIND_SHIFT;
    return *this;
  }

  // Make the function constructible.
  FunctionFlags& setIsConstructor() {
    MOZ_ASSERT(!isConstructor());
    MOZ_ASSERT(isSelfHostedBuiltin());
    return setFlags(CONSTRUCTOR);
  }

  FunctionFlags& setIsSelfHostedBuiltin() {
    MOZ_ASSERT(isInterpreted());
    MOZ_ASSERT(!isSelfHostedBuiltin());
    setFlags(SELF_HOSTED);
    // Self-hosted functions should not be constructable.
    return clearFlags(CONSTRUCTOR);
  }
  FunctionFlags& setIsIntrinsic() {
    MOZ_ASSERT(isNativeFun());
    MOZ_ASSERT(!isIntrinsic());
    return setFlags(SELF_HOSTED);
  }

  FunctionFlags& setResolvedLength() { return setFlags(RESOLVED_LENGTH); }
  FunctionFlags& setResolvedName() { return setFlags(RESOLVED_NAME); }

  FunctionFlags& setInferredName() { return setFlags(HAS_INFERRED_NAME); }

  FunctionFlags& setGuessedAtom() { return setFlags(HAS_GUESSED_ATOM); }

  FunctionFlags& setSelfHostedLazy() { return setFlags(SELFHOSTLAZY); }
  FunctionFlags& clearSelfHostedLazy() { return clearFlags(SELFHOSTLAZY); }
  FunctionFlags& setBaseScript() { return setFlags(BASESCRIPT); }
  FunctionFlags& clearBaseScript() { return clearFlags(BASESCRIPT); }

  FunctionFlags& clearLazyAccessorName() {
    return clearFlags(LAZY_ACCESSOR_NAME);
  }

  FunctionFlags& setNativeJitEntry() { return setFlags(NATIVE_JIT_ENTRY); }

  bool isExtended() const { return hasFlags(EXTENDED); }
  FunctionFlags& setIsExtended() { return setFlags(EXTENDED); }

  bool isNativeConstructor() const { return hasFlags(NATIVE_CTOR); }

  FunctionFlags& setIsGhost() { return setFlags(GHOST_FUNCTION); }
  bool isGhost() const { return hasFlags(GHOST_FUNCTION); }

  static constexpr uint16_t HasJitEntryFlags() {
    return BASESCRIPT | SELFHOSTLAZY | NATIVE_JIT_ENTRY;
  }

  static FunctionFlags clearMutableflags(FunctionFlags flags) {
    return FunctionFlags(flags.toRaw() & ~FunctionFlags::MUTABLE_FLAGS);
  }
};

} /* namespace js */

#endif /* vm_FunctionFlags_h */
