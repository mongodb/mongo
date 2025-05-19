/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSFunction_h
#define vm_JSFunction_h

/*
 * JS function definitions.
 */

#include <string_view>

#include "jstypes.h"

#include "gc/Policy.h"
#include "js/shadow/Function.h"        // JS::shadow::Function
#include "vm/FunctionFlags.h"          // FunctionFlags
#include "vm/FunctionPrefixKind.h"     // FunctionPrefixKind
#include "vm/GeneratorAndAsyncKind.h"  // GeneratorKind, FunctionAsyncKind
#include "vm/JSAtomUtils.h"            // AtomIsMarked
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "wasm/WasmTypeDef.h"

class JSJitInfo;

namespace js {

class FunctionExtended;
class JS_PUBLIC_API GenericPrinter;
class JSONPrinter;
struct SelfHostedLazyScript;

using Native = JSNative;

static constexpr std::string_view FunctionConstructorMedialSigils = ") {\n";
static constexpr std::string_view FunctionConstructorFinalBrace = "\n}";

// JSFunctions can have one of two classes:
extern const JSClass FunctionClass;
extern const JSClass ExtendedFunctionClass;

namespace wasm {

class Instance;

}  // namespace wasm
}  // namespace js

class JSFunction : public js::NativeObject {
 public:
  static_assert(sizeof(js::FunctionFlags) == sizeof(uint16_t));
  static constexpr size_t ArgCountShift = 16;
  static constexpr size_t FlagsMask = js::BitMask(ArgCountShift);
  static constexpr size_t ArgCountMask = js::BitMask(16) << ArgCountShift;

  enum {
    /*
     * Bitfield composed of FunctionFlags and argument count, stored as a
     * PrivateUint32Value.
     *
     * If any of these flags needs to be accessed in off-thread JIT compilation,
     * copy it to js::jit::WrappedFunction.
     */
    FlagsAndArgCountSlot,

    /*
     * For native functions, the native method pointer stored as a private
     * value, or undefined.
     *
     * For interpreted functions, the environment object for new activations or
     * null.
     */
    NativeFuncOrInterpretedEnvSlot,

    /*
     * For native functions this is one of:
     *
     *  - JSJitInfo* to be used by the JIT, only used if isBuiltinNative() for
     *    builtin natives
     *
     *  - wasm function index for wasm/asm.js without a jit entry. Always has
     *    the low bit set to ensure it's never identical to a BaseScript*
     *    pointer
     *
     *  - a native JIT entry (used for Wasm and TrampolineNative functions)
     *
     * The JIT depends on none of the above being a valid BaseScript pointer.
     *
     * For interpreted functions this is either a BaseScript or the
     * SelfHostedLazyScript pointer.
     *
     * These are all stored as private values, because the JIT assumes that it
     * can access the SelfHostedLazyScript and BaseScript pointer in the same
     * way.
     */
    NativeJitInfoOrInterpretedScriptSlot,

    // The `atom_` field can have different meanings depending on the function
    // type and flags. It is used for diagnostics, decompiling, and
    //
    //   a. If LAZY_ACCESSOR_NAME is set, to store the initial value of the
    //      unprefixed part of "name" property of a accessor function.
    //      But also see RESOLVED_NAME.
    //   b. If HAS_GUESSED_ATOM is not set, to store the initial value of the
    //      "name" property of functions. But also see RESOLVED_NAME.
    //   c. If HAS_GUESSED_ATOM is set, `atom_` is only used for diagnostics,
    //      but must not be used for the "name" property.
    //   d. If HAS_INFERRED_NAME is set, the function wasn't given an explicit
    //      name in the source text, e.g. `function fn(){}`, but instead it
    //      was inferred based on how the function was defined in the source
    //      text. The exact name inference rules are defined in the ECMAScript
    //      specification.
    //      Name inference can happen at compile-time, for example in
    //      `var fn = function(){}`, or it can happen at runtime, for example
    //      in `var o = {[Symbol.iterator]: function(){}}`. When it happens at
    //      compile-time, the HAS_INFERRED_NAME is set directly in the
    //      bytecode emitter, when it happens at runtime, the flag is set when
    //      evaluating the JSOp::SetFunName bytecode.
    //   e. HAS_GUESSED_ATOM,  HAS_INFERRED_NAME, and LAZY_ACCESSOR_NAME are
    //      mutually exclusive and cannot be set at the same time.
    //   f. `atom_` can be null if neither an explicit, nor inferred, nor a
    //      guessed name was set.
    //
    // Self-hosted functions have two names. For example, Array.prototype.sort
    // has the standard name "sort", but the implementation in Array.js is named
    // "ArraySort".
    //
    // -   In the self-hosting realm, these functions have `_atom` set to the
    //     implementation name.
    //
    // -   When we clone these functions into normal realms, we set `_atom` to
    //     the standard name. (The self-hosted name is also stored on the clone,
    //     in another slot; see GetClonedSelfHostedFunctionName().)
    AtomSlot,

    SlotCount
  };

 private:
  using FunctionFlags = js::FunctionFlags;

 public:
  static inline JSFunction* create(JSContext* cx, js::gc::AllocKind kind,
                                   js::gc::Heap heap,
                                   js::Handle<js::SharedShape*> shape);

  /* Call objects must be created for each invocation of this function. */
  bool needsCallObject() const;

  bool needsExtraBodyVarEnvironment() const;
  bool needsNamedLambdaEnvironment() const;

  bool needsFunctionEnvironmentObjects() const {
    bool res = nonLazyScript()->needsFunctionEnvironmentObjects();
    MOZ_ASSERT(res == (needsCallObject() || needsNamedLambdaEnvironment()));
    return res;
  }

  bool needsSomeEnvironmentObject() const {
    return needsFunctionEnvironmentObjects() || needsExtraBodyVarEnvironment();
  }

  uint32_t flagsAndArgCountRaw() const {
    return getFixedSlot(FlagsAndArgCountSlot).toPrivateUint32();
  }

  void initFlagsAndArgCount() {
    initFixedSlot(FlagsAndArgCountSlot, JS::PrivateUint32Value(0));
  }

  size_t nargs() const { return flagsAndArgCountRaw() >> ArgCountShift; }

  FunctionFlags flags() const {
    return FunctionFlags(uint16_t(flagsAndArgCountRaw() & FlagsMask));
  }

  FunctionFlags::FunctionKind kind() const { return flags().kind(); }

#ifdef DEBUG
  void assertFunctionKindIntegrity() { flags().assertFunctionKindIntegrity(); }
#endif

  /* A function can be classified as either native (C++) or interpreted (JS): */
  bool isInterpreted() const { return flags().isInterpreted(); }
  bool isNativeFun() const { return flags().isNativeFun(); }

  bool isConstructor() const { return flags().isConstructor(); }

  bool isNonBuiltinConstructor() const {
    return flags().isNonBuiltinConstructor();
  }

  /* Possible attributes of a native function: */
  bool isAsmJSNative() const { return flags().isAsmJSNative(); }

  bool isWasm() const { return flags().isWasm(); }
  bool isWasmWithJitEntry() const { return flags().isWasmWithJitEntry(); }
  bool isNativeWithJitEntry() const { return flags().isNativeWithJitEntry(); }
  bool isNativeWithoutJitEntry() const {
    return flags().isNativeWithoutJitEntry();
  }
  bool isBuiltinNative() const { return flags().isBuiltinNative(); }

  bool hasJitEntry() const { return flags().hasJitEntry(); }

  /* Possible attributes of an interpreted function: */
  bool hasInferredName() const { return flags().hasInferredName(); }
  bool hasGuessedAtom() const { return flags().hasGuessedAtom(); }

  bool isLambda() const { return flags().isLambda(); }

  // These methods determine which kind of script we hold.
  //
  // For live JSFunctions the pointer values will always be non-null, but due to
  // partial initialization the GC (and other features that scan the heap
  // directly) may still return a null pointer.
  bool hasSelfHostedLazyScript() const {
    return flags().hasSelfHostedLazyScript();
  }
  bool hasBaseScript() const { return flags().hasBaseScript(); }

  bool hasBytecode() const {
    MOZ_ASSERT(!isIncomplete());
    return hasBaseScript() && baseScript()->hasBytecode();
  }

  bool isGhost() const { return flags().isGhost(); }

  // Arrow functions store their lexical new.target in the first extended slot.
  bool isArrow() const { return flags().isArrow(); }
  // Every class-constructor is also a method.
  bool isMethod() const { return flags().isMethod(); }
  bool isClassConstructor() const { return flags().isClassConstructor(); }

  bool isGetter() const { return flags().isGetter(); }
  bool isSetter() const { return flags().isSetter(); }

  bool isAccessorWithLazyName() const {
    return flags().isAccessorWithLazyName();
  }

  bool allowSuperProperty() const { return flags().allowSuperProperty(); }

  bool hasResolvedLength() const { return flags().hasResolvedLength(); }
  bool hasResolvedName() const { return flags().hasResolvedName(); }

  bool isSelfHostedOrIntrinsic() const {
    return flags().isSelfHostedOrIntrinsic();
  }
  bool isSelfHostedBuiltin() const { return flags().isSelfHostedBuiltin(); }

  bool isIntrinsic() const { return flags().isIntrinsic(); }

  bool hasJitScript() const {
    if (!hasBaseScript()) {
      return false;
    }

    return baseScript()->hasJitScript();
  }

  /* Compound attributes: */
  bool isBuiltin() const { return isBuiltinNative() || isSelfHostedBuiltin(); }

  bool isNamedLambda() const {
    return flags().isNamedLambda(maybePartialDisplayAtom() != nullptr);
  }

  bool hasLexicalThis() const { return isArrow(); }

  bool isBuiltinFunctionConstructor();
  bool needsPrototypeProperty();

  // Returns true if this function must have a non-configurable .prototype data
  // property. This is used to ensure looking up .prototype elsewhere will have
  // no side-effects.
  bool hasNonConfigurablePrototypeDataProperty();

  // Returns true if |new Fun()| should not allocate a new object caller-side
  // but pass the uninitialized-lexical MagicValue and rely on the callee to
  // construct its own |this| object.
  bool constructorNeedsUninitializedThis() const {
    MOZ_ASSERT(isConstructor());
    MOZ_ASSERT(isInterpreted());
    return isDerivedClassConstructor();
  }

  /* Returns the strictness of this function, which must be interpreted. */
  bool strict() const { return baseScript()->strict(); }

  void setFlags(FunctionFlags flags) { setFlags(flags.toRaw()); }
  void setFlags(uint16_t flags) {
    uint32_t flagsAndArgCount = flagsAndArgCountRaw();
    flagsAndArgCount &= ~FlagsMask;
    flagsAndArgCount |= flags;
    js::HeapSlot& slot = getFixedSlotRef(FlagsAndArgCountSlot);
    slot.unbarrieredSet(JS::PrivateUint32Value(flagsAndArgCount));
  }

  // Make the function constructible.
  void setIsConstructor() { setFlags(flags().setIsConstructor()); }

  // Can be called multiple times by the parser.
  void setArgCount(uint16_t nargs) {
    uint32_t flagsAndArgCount = flagsAndArgCountRaw();
    flagsAndArgCount &= ~ArgCountMask;
    flagsAndArgCount |= nargs << ArgCountShift;
    js::HeapSlot& slot = getFixedSlotRef(FlagsAndArgCountSlot);
    slot.unbarrieredSet(JS::PrivateUint32Value(flagsAndArgCount));
  }

  void setIsSelfHostedBuiltin() { setFlags(flags().setIsSelfHostedBuiltin()); }
  void setIsIntrinsic() { setFlags(flags().setIsIntrinsic()); }

  void setResolvedLength() { setFlags(flags().setResolvedLength()); }
  void setResolvedName() { setFlags(flags().setResolvedName()); }

  static inline bool getUnresolvedLength(JSContext* cx, js::HandleFunction fun,
                                         uint16_t* length);

  // Returns the function's unresolved name.
  // Returns an empty string if the function doesn't have name.
  // Returns nullptr when OOM happens.
  inline JSAtom* getUnresolvedName(JSContext* cx);

  // Returns the function's unresolved name.
  // Returns an empty string if the function doesn't have name.
  inline JSAtom* infallibleGetUnresolvedName(JSContext* cx);

  // Returns the name of an accessor function with lazy name.
  JSAtom* getAccessorNameForLazy(JSContext* cx);

  // Returns the function's name expclitly specified as syntax, or
  // passed when creating a native function.
  //
  // Returns true and *name!=nullptr if the function has an explicit name.
  // Returns true and *name==nullptr if the function doesn't have an explicit
  // name.
  // Returns false if OOM happens.
  bool getExplicitName(JSContext* cx, JS::MutableHandle<JSAtom*> name);

  // Almost same as getExplicitName.
  //
  // Returns non-nullptr if the function has an explicit name.
  // Returns nullptr if the function doesn't have an explicit name.
  //
  // If this function has lazy name, this returns partial name, such as the
  // function name without "get " or "set " prefix.
  JSAtom* maybePartialExplicitName() const {
    return (hasInferredName() || hasGuessedAtom()) ? nullptr : rawAtom();
  }

  // Same as maybePartialExplicitName, except for asserting this function
  // doesn't have lazy name.
  //
  // This can be used e.g. when this function is known to be scripted.
  JSAtom* fullExplicitName() const {
    MOZ_ASSERT(!isAccessorWithLazyName());
    return (hasInferredName() || hasGuessedAtom()) ? nullptr : rawAtom();
  }

  JSAtom* fullExplicitOrInferredName() const {
    MOZ_ASSERT(!isAccessorWithLazyName());
    return hasGuessedAtom() ? nullptr : rawAtom();
  }

  void initAtom(JSAtom* atom) {
    MOZ_ASSERT_IF(atom, js::AtomIsMarked(zone(), atom));
    MOZ_ASSERT(getFixedSlot(AtomSlot).isUndefined());
    if (atom) {
      initFixedSlot(AtomSlot, JS::StringValue(atom));
    }
  }

  void setAtom(JSAtom* atom) {
    MOZ_ASSERT_IF(atom, js::AtomIsMarked(zone(), atom));
    setFixedSlot(AtomSlot, atom ? JS::StringValue(atom) : JS::UndefinedValue());
  }

  // Returns the function's name which can be used for informative purpose.
  //
  // Returns true and *name!=nullptr if the function has a name.
  // Returns true and *name==nullptr if the function doesn't have a name.
  // Returns false if OOM happens.
  bool getDisplayAtom(JSContext* cx, JS::MutableHandle<JSAtom*> name);

  // Almost same as getDisplayAtom.
  //
  // Returns non-nullptr if the function has a name.
  // Returns nullptr if the function doesn't have a name.
  //
  // If this function has lazy name, this returns partial name, such as the
  // function name without "get " or "set " prefix.
  JSAtom* maybePartialDisplayAtom() const { return rawAtom(); }

  // Same as maybePartialDisplayAtom, except for asserting this function
  // doesn't have lazy name.
  //
  // This can be used e.g. when this function is known to be scripted.
  JSAtom* fullDisplayAtom() const {
    MOZ_ASSERT(!isAccessorWithLazyName());
    return rawAtom();
  }

  JSAtom* rawAtom() const {
    JS::Value value = getFixedSlot(AtomSlot);
    return value.isUndefined() ? nullptr : &value.toString()->asAtom();
  }

  void setInferredName(JSAtom* atom) {
    MOZ_ASSERT(!rawAtom());
    MOZ_ASSERT(atom);
    MOZ_ASSERT(!hasGuessedAtom());
    setAtom(atom);
    setFlags(flags().setInferredName());
  }
  JSAtom* inferredName() const {
    MOZ_ASSERT(hasInferredName());
    MOZ_ASSERT(rawAtom());
    return rawAtom();
  }

  void setGuessedAtom(JSAtom* atom) {
    MOZ_ASSERT(!rawAtom());
    MOZ_ASSERT(atom);
    MOZ_ASSERT(!hasInferredName());
    MOZ_ASSERT(!hasGuessedAtom());
    setAtom(atom);
    setFlags(flags().setGuessedAtom());
  }

  /* uint16_t representation bounds number of call object dynamic slots. */
  enum { MAX_ARGS_AND_VARS = 2 * ((1U << 16) - 1) };

  /*
   * For an interpreted function, accessors for the initial scope object of
   * activations (stack frames) of the function.
   */
  JSObject* environment() const {
    MOZ_ASSERT(isInterpreted());
    return getFixedSlot(NativeFuncOrInterpretedEnvSlot).toObjectOrNull();
  }

  void initEnvironment(JSObject* obj) {
    MOZ_ASSERT(isInterpreted());
    initFixedSlot(NativeFuncOrInterpretedEnvSlot, JS::ObjectOrNullValue(obj));
  }

 public:
  static constexpr size_t offsetOfFlagsAndArgCount() {
    return getFixedSlotOffset(FlagsAndArgCountSlot);
  }
  static size_t offsetOfEnvironment() { return offsetOfNativeOrEnv(); }
  static size_t offsetOfAtom() { return getFixedSlotOffset(AtomSlot); }

  static bool delazifyLazilyInterpretedFunction(JSContext* cx,
                                                js::HandleFunction fun);
  static bool delazifySelfHostedLazyFunction(JSContext* cx,
                                             js::HandleFunction fun);
  void maybeRelazify(JSRuntime* rt);

  // Function Scripts
  //
  // Interpreted functions have either a BaseScript or a SelfHostedLazyScript. A
  // BaseScript may either be lazy or non-lazy (hasBytecode()). Methods may
  // return a JSScript* if underlying BaseScript is known to have bytecode.
  //
  // There are several methods to get the script of an interpreted function:
  //
  // - For all interpreted functions, getOrCreateScript() will get the
  //   JSScript, delazifying the function if necessary. This is the safest to
  //   use, but has extra checks, requires a cx and may trigger a GC.
  //
  // - For functions known to have a JSScript, nonLazyScript() will get it.

  static JSScript* getOrCreateScript(JSContext* cx, js::HandleFunction fun) {
    MOZ_ASSERT(fun->isInterpreted());
    MOZ_ASSERT(cx);

    if (fun->hasSelfHostedLazyScript()) {
      if (!delazifySelfHostedLazyFunction(cx, fun)) {
        return nullptr;
      }
      return fun->nonLazyScript();
    }

    MOZ_ASSERT(fun->hasBaseScript());

    if (!fun->baseScript()->hasBytecode()) {
      if (!delazifyLazilyInterpretedFunction(cx, fun)) {
        return nullptr;
      }
    }
    return fun->nonLazyScript();
  }

  // If this is a scripted function, returns its canonical function (the
  // original function allocated by the frontend). Note that lazy self-hosted
  // builtins don't have a lazy script so in that case we also return nullptr.
  JSFunction* maybeCanonicalFunction() const {
    if (hasBaseScript()) {
      return baseScript()->function();
    }
    return nullptr;
  }

 private:
  void* nativeJitInfoOrInterpretedScript() const {
    return getFixedSlot(NativeJitInfoOrInterpretedScriptSlot).toPrivate();
  }
  void setNativeJitInfoOrInterpretedScript(void* ptr) {
    // This always stores a PrivateValue and so doesn't require a barrier.
    js::HeapSlot& slot = getFixedSlotRef(NativeJitInfoOrInterpretedScriptSlot);
    slot.unbarrieredSet(JS::PrivateValue(ptr));
  }

 public:
  // The default state of a JSFunction that is not ready for execution. If
  // observed outside initialization, this is the result of failure during
  // bytecode compilation.
  //
  // A BaseScript is fully initialized before u.script.s.script_ is initialized
  // with a reference to it.
  bool isIncomplete() const {
    return isInterpreted() && !nativeJitInfoOrInterpretedScript();
  }

  JSScript* nonLazyScript() const {
    MOZ_ASSERT(hasBytecode());
    return static_cast<JSScript*>(baseScript());
  }

  js::SelfHostedLazyScript* selfHostedLazyScript() const {
    MOZ_ASSERT(hasSelfHostedLazyScript());
    return static_cast<js::SelfHostedLazyScript*>(
        nativeJitInfoOrInterpretedScript());
  }

  // Access fields defined on both lazy and non-lazy scripts.
  js::BaseScript* baseScript() const {
    MOZ_ASSERT(hasBaseScript());
    return static_cast<JSScript*>(nativeJitInfoOrInterpretedScript());
  }

  static inline bool getLength(JSContext* cx, js::HandleFunction fun,
                               uint16_t* length);

  js::Scope* enclosingScope() const { return baseScript()->enclosingScope(); }

  void setEnclosingLazyScript(js::BaseScript* enclosingScript) {
    baseScript()->setEnclosingScript(enclosingScript);
  }

  js::GeneratorKind generatorKind() const {
    if (hasBaseScript()) {
      return baseScript()->generatorKind();
    }
    if (hasSelfHostedLazyScript()) {
      return clonedSelfHostedGeneratorKind();
    }
    return js::GeneratorKind::NotGenerator;
  }

  js::GeneratorKind clonedSelfHostedGeneratorKind() const;

  bool isGenerator() const {
    return generatorKind() == js::GeneratorKind::Generator;
  }

  js::FunctionAsyncKind asyncKind() const {
    if (hasBaseScript()) {
      return baseScript()->asyncKind();
    }
    return js::FunctionAsyncKind::SyncFunction;
  }

  bool isAsync() const {
    return asyncKind() == js::FunctionAsyncKind::AsyncFunction;
  }

  bool isGeneratorOrAsync() const { return isGenerator() || isAsync(); }

  void initScript(js::BaseScript* script) {
    MOZ_ASSERT_IF(script, realm() == script->realm());
    MOZ_ASSERT(isInterpreted());
    MOZ_ASSERT_IF(hasBaseScript(),
                  !baseScript());  // No write barrier required.
    setNativeJitInfoOrInterpretedScript(script);
  }

  void initSelfHostedLazyScript(js::SelfHostedLazyScript* lazy) {
    MOZ_ASSERT(isSelfHostedBuiltin());
    MOZ_ASSERT(isInterpreted());
    if (hasBaseScript()) {
      js::gc::PreWriteBarrier(baseScript());
    }
    FunctionFlags f = flags();
    f.clearBaseScript();
    f.setSelfHostedLazy();
    setFlags(f);
    setNativeJitInfoOrInterpretedScript(lazy);
    MOZ_ASSERT(hasSelfHostedLazyScript());
  }

  void clearSelfHostedLazyScript() {
    MOZ_ASSERT(isSelfHostedBuiltin());
    MOZ_ASSERT(isInterpreted());
    MOZ_ASSERT(!hasBaseScript());  // No write barrier required.
    FunctionFlags f = flags();
    f.clearSelfHostedLazy();
    f.setBaseScript();
    setFlags(f);
    setNativeJitInfoOrInterpretedScript(nullptr);
    MOZ_ASSERT(isIncomplete());
  }

  JSNative native() const {
    MOZ_ASSERT(isNativeFun());
    return nativeUnchecked();
  }
  JSNative nativeUnchecked() const {
    // Can be called by Ion off-main thread.
    JS::Value value = getFixedSlot(NativeFuncOrInterpretedEnvSlot);
    return reinterpret_cast<JSNative>(value.toPrivate());
  }

  JSNative maybeNative() const { return isInterpreted() ? nullptr : native(); }

  void initNative(js::Native native, const JSJitInfo* jitInfo) {
    MOZ_ASSERT(isNativeFun());
    MOZ_ASSERT_IF(jitInfo, isBuiltinNative());
    MOZ_ASSERT(native);
    initFixedSlot(NativeFuncOrInterpretedEnvSlot,
                  JS::PrivateValue(reinterpret_cast<void*>(native)));
    setNativeJitInfoOrInterpretedScript(const_cast<JSJitInfo*>(jitInfo));
  }
  bool hasJitInfo() const {
    return flags().canHaveJitInfo() && jitInfoUnchecked();
  }
  const JSJitInfo* jitInfo() const {
    MOZ_ASSERT(hasJitInfo());
    return jitInfoUnchecked();
  }
  const JSJitInfo* jitInfoUnchecked() const {
    // Can be called by Ion off-main thread.
    return static_cast<const JSJitInfo*>(nativeJitInfoOrInterpretedScript());
  }
  void setJitInfo(const JSJitInfo* data) {
    MOZ_ASSERT(isBuiltinNative());
    MOZ_ASSERT(data);
    setNativeJitInfoOrInterpretedScript(const_cast<JSJitInfo*>(data));
  }

  // wasm functions are always natives and either:
  //  - store a function-index in u.n.extra and can only be called through the
  //    fun->native() entry point from C++.
  //  - store a jit-entry code pointer in u.n.extra and can be called by jit
  //    code directly. C++ callers can still use the fun->native() entry point
  //    (computing the function index from the jit-entry point).
  void setWasmFuncIndex(uint32_t funcIndex) {
    MOZ_ASSERT(isWasm() || isAsmJSNative());
    MOZ_ASSERT(!isWasmWithJitEntry());
    MOZ_ASSERT(!nativeJitInfoOrInterpretedScript());
    // See wasmFuncIndex_ comment for why we set the low bit.
    uintptr_t tagged = (uintptr_t(funcIndex) << 1) | 1;
    setNativeJitInfoOrInterpretedScript(reinterpret_cast<void*>(tagged));
  }
  uint32_t wasmFuncIndex() const {
    MOZ_ASSERT(isWasm() || isAsmJSNative());
    MOZ_ASSERT(!isWasmWithJitEntry());
    uintptr_t tagged = uintptr_t(nativeJitInfoOrInterpretedScript());
    MOZ_ASSERT(tagged & 1);
    return tagged >> 1;
  }
  void setWasmJitEntry(void** entry) {
    MOZ_ASSERT(*entry);
    MOZ_ASSERT(isWasm());
    MOZ_ASSERT(!isWasmWithJitEntry());
    setFlags(flags().setNativeJitEntry());
    setNativeJitInfoOrInterpretedScript(entry);
    MOZ_ASSERT(isWasmWithJitEntry());
  }
  void setTrampolineNativeJitEntry(void** entry) {
    MOZ_ASSERT(*entry);
    MOZ_ASSERT(isBuiltinNative());
    MOZ_ASSERT(!hasJitEntry());
    MOZ_ASSERT(!hasJitInfo(), "shouldn't clobber JSJitInfo");
    setFlags(flags().setNativeJitEntry());
    setNativeJitInfoOrInterpretedScript(entry);
    MOZ_ASSERT(isNativeWithJitEntry());
  }
  void** wasmJitEntry() const {
    MOZ_ASSERT(isWasmWithJitEntry());
    return nativeJitEntry();
  }
  void** nativeJitEntry() const {
    MOZ_ASSERT(isNativeWithJitEntry());
    return static_cast<void**>(nativeJitInfoOrInterpretedScript());
  }
  inline js::wasm::Instance& wasmInstance() const;
  inline js::wasm::SuperTypeVector& wasmSuperTypeVector() const;
  inline const js::wasm::TypeDef* wasmTypeDef() const;

  bool isDerivedClassConstructor() const;
  bool isSyntheticFunction() const;

  static unsigned offsetOfNativeOrEnv() {
    return getFixedSlotOffset(NativeFuncOrInterpretedEnvSlot);
  }
  static unsigned offsetOfJitInfoOrScript() {
    return getFixedSlotOffset(NativeJitInfoOrInterpretedScriptSlot);
  }

  inline void trace(JSTracer* trc);

 public:
  inline bool isExtended() const {
    bool extended = flags().isExtended();
    MOZ_ASSERT_IF(isTenured(),
                  extended == (asTenured().getAllocKind() ==
                               js::gc::AllocKind::FUNCTION_EXTENDED));
    return extended;
  }

  /*
   * Accessors for data stored in extended functions. Use setExtendedSlot if the
   * function has already been initialized. Otherwise use initExtendedSlot.
   */
  inline void initExtendedSlot(uint32_t slot, const js::Value& val);
  inline void setExtendedSlot(uint32_t slot, const js::Value& val);
  inline const js::Value& getExtendedSlot(uint32_t slot) const;

  /* GC support. */
  js::gc::AllocKind getAllocKind() const {
    static_assert(
        js::gc::AllocKind::FUNCTION != js::gc::AllocKind::FUNCTION_EXTENDED,
        "extended/non-extended AllocKinds have to be different "
        "for getAllocKind() to have a reason to exist");

    js::gc::AllocKind kind = js::gc::AllocKind::FUNCTION;
    if (isExtended()) {
      kind = js::gc::AllocKind::FUNCTION_EXTENDED;
    }
    MOZ_ASSERT_IF(isTenured(), kind == asTenured().getAllocKind());
    return kind;
  }

  // If we're constructing with this function, choose an appropriate
  // allocKind.
  static bool getAllocKindForThis(JSContext* cx, js::HandleFunction func,
                                  js::gc::AllocKind& allocKind);

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dumpOwnFields(js::JSONPrinter& json) const;
  void dumpOwnStringContent(js::GenericPrinter& out) const;
#endif
};

static_assert(sizeof(JSFunction) == sizeof(JS::shadow::Function),
              "shadow interface must match actual interface");

static_assert(unsigned(JSFunction::FlagsAndArgCountSlot) ==
              unsigned(JS::shadow::Function::FlagsAndArgCountSlot));
static_assert(unsigned(JSFunction::NativeFuncOrInterpretedEnvSlot) ==
              unsigned(JS::shadow::Function::NativeFuncOrInterpretedEnvSlot));
static_assert(
    unsigned(JSFunction::NativeJitInfoOrInterpretedScriptSlot) ==
    unsigned(JS::shadow::Function::NativeJitInfoOrInterpretedScriptSlot));
static_assert(unsigned(JSFunction::AtomSlot) ==
              unsigned(JS::shadow::Function::AtomSlot));

extern JSString* fun_toStringHelper(JSContext* cx, js::HandleObject obj,
                                    bool isToSource);

namespace js {

extern bool Function(JSContext* cx, unsigned argc, Value* vp);

extern bool Generator(JSContext* cx, unsigned argc, Value* vp);

extern bool AsyncFunctionConstructor(JSContext* cx, unsigned argc, Value* vp);

extern bool AsyncGeneratorConstructor(JSContext* cx, unsigned argc, Value* vp);

// If enclosingEnv is null, the function will have a null environment()
// (yes, null, not the global lexical environment).  In all cases, the global
// will be used as the terminating environment.

extern JSFunction* NewFunctionWithProto(
    JSContext* cx, JSNative native, unsigned nargs, FunctionFlags flags,
    HandleObject enclosingEnv, Handle<JSAtom*> atom, HandleObject proto,
    gc::AllocKind allocKind = gc::AllocKind::FUNCTION,
    NewObjectKind newKind = GenericObject);

// Allocate a new function backed by a JSNative.  Note that by default this
// creates a tenured object.
inline JSFunction* NewNativeFunction(
    JSContext* cx, JSNative native, unsigned nargs, Handle<JSAtom*> atom,
    gc::AllocKind allocKind = gc::AllocKind::FUNCTION,
    NewObjectKind newKind = TenuredObject,
    FunctionFlags flags = FunctionFlags::NATIVE_FUN) {
  MOZ_ASSERT(native);
  return NewFunctionWithProto(cx, native, nargs, flags, nullptr, atom, nullptr,
                              allocKind, newKind);
}

// Allocate a new constructor backed by a JSNative.  Note that by default this
// creates a tenured object.
inline JSFunction* NewNativeConstructor(
    JSContext* cx, JSNative native, unsigned nargs, Handle<JSAtom*> atom,
    gc::AllocKind allocKind = gc::AllocKind::FUNCTION,
    NewObjectKind newKind = TenuredObject,
    FunctionFlags flags = FunctionFlags::NATIVE_CTOR) {
  MOZ_ASSERT(native);
  MOZ_ASSERT(flags.isNativeConstructor());
  return NewFunctionWithProto(cx, native, nargs, flags, nullptr, atom, nullptr,
                              allocKind, newKind);
}

// Determine which [[Prototype]] to use when creating a new function using the
// requested generator and async kind.
//
// This sets `proto` to `nullptr` for non-generator, synchronous functions to
// mean "the builtin %FunctionPrototype% in the current realm", the common case.
//
// We could set it to `cx->global()->getOrCreateFunctionPrototype()`, but
// nullptr gets a fast path in e.g. js::NewObjectWithClassProtoCommon.
extern bool GetFunctionPrototype(JSContext* cx, js::GeneratorKind generatorKind,
                                 js::FunctionAsyncKind asyncKind,
                                 js::MutableHandleObject proto);

extern JSAtom* IdToFunctionName(
    JSContext* cx, HandleId id,
    FunctionPrefixKind prefixKind = FunctionPrefixKind::None);

extern bool SetFunctionName(JSContext* cx, HandleFunction fun, HandleValue name,
                            FunctionPrefixKind prefixKind);

extern JSFunction* DefineFunction(
    JSContext* cx, HandleObject obj, HandleId id, JSNative native,
    unsigned nargs, unsigned flags,
    gc::AllocKind allocKind = gc::AllocKind::FUNCTION);

extern bool fun_toString(JSContext* cx, unsigned argc, Value* vp);

extern void ThrowTypeErrorBehavior(JSContext* cx);

/*
 * Function extended with reserved slots for use by various kinds of functions.
 * Most functions do not have these extensions, but enough do that efficient
 * storage is required (no malloc'ed reserved slots).
 */
class FunctionExtended : public JSFunction {
 public:
  enum {
    FirstExtendedSlot = JSFunction::SlotCount,
    SecondExtendedSlot,
    ThirdExtendedSlot,

    SlotCount
  };

  static const uint32_t NUM_EXTENDED_SLOTS = 3;

  static const uint32_t METHOD_HOMEOBJECT_SLOT = 0;

  // wasm/asm.js exported functions store a code pointer to their direct entry
  // point (see CodeRange::funcUncheckedCallEntry()) to support the call_ref
  // instruction.
  static const uint32_t WASM_FUNC_UNCHECKED_ENTRY_SLOT = 0;

  // wasm/asm.js exported functions store the wasm::Instance pointer of their
  // instance.
  static const uint32_t WASM_INSTANCE_SLOT = 1;

  // wasm/asm.js exported functions store a pointer to their
  // wasm::SuperTypeVector for downcasting.
  static const uint32_t WASM_STV_SLOT = 2;

  // asm.js module functions store their WasmModuleObject in the first slot.
  static const uint32_t ASMJS_MODULE_SLOT = 0;

  // Async module callback handlers store their ModuleObject in the first slot.
  static const uint32_t MODULE_SLOT = 0;

  static inline size_t offsetOfExtendedSlot(uint32_t which) {
    MOZ_ASSERT(which < NUM_EXTENDED_SLOTS);
    return getFixedSlotOffset(FirstExtendedSlot + which);
  }
  static inline size_t offsetOfMethodHomeObjectSlot() {
    return offsetOfExtendedSlot(METHOD_HOMEOBJECT_SLOT);
  }
  static inline size_t offsetOfWasmSTV() {
    return offsetOfExtendedSlot(WASM_STV_SLOT);
  }

 private:
  friend class JSFunction;
};

extern JSFunction* CloneFunctionReuseScript(JSContext* cx, HandleFunction fun,
                                            HandleObject enclosingEnv,
                                            HandleObject proto);

extern JSFunction* CloneAsmJSModuleFunction(JSContext* cx, HandleFunction fun);

}  // namespace js

template <>
inline bool JSObject::is<JSFunction>() const {
  return getClass()->isJSFunction();
}

inline void JSFunction::initExtendedSlot(uint32_t which, const js::Value& val) {
  MOZ_ASSERT(isExtended());
  MOZ_ASSERT(which < js::FunctionExtended::NUM_EXTENDED_SLOTS);
  MOZ_ASSERT(js::IsObjectValueInCompartment(val, compartment()));
  initFixedSlot(js::FunctionExtended::FirstExtendedSlot + which, val);
}

inline void JSFunction::setExtendedSlot(uint32_t which, const js::Value& val) {
  MOZ_ASSERT(isExtended());
  MOZ_ASSERT(which < js::FunctionExtended::NUM_EXTENDED_SLOTS);
  MOZ_ASSERT(js::IsObjectValueInCompartment(val, compartment()));
  setFixedSlot(js::FunctionExtended::FirstExtendedSlot + which, val);
}

inline const js::Value& JSFunction::getExtendedSlot(uint32_t which) const {
  MOZ_ASSERT(isExtended());
  MOZ_ASSERT(which < js::FunctionExtended::NUM_EXTENDED_SLOTS);
  return getFixedSlot(js::FunctionExtended::FirstExtendedSlot + which);
}

inline js::wasm::Instance& JSFunction::wasmInstance() const {
  MOZ_ASSERT(isWasm() || isAsmJSNative());
  MOZ_ASSERT(
      !getExtendedSlot(js::FunctionExtended::WASM_INSTANCE_SLOT).isUndefined());
  return *static_cast<js::wasm::Instance*>(
      getExtendedSlot(js::FunctionExtended::WASM_INSTANCE_SLOT).toPrivate());
}

inline js::wasm::SuperTypeVector& JSFunction::wasmSuperTypeVector() const {
  MOZ_ASSERT(isWasm());
  MOZ_ASSERT(
      !getExtendedSlot(js::FunctionExtended::WASM_STV_SLOT).isUndefined());
  return *static_cast<js::wasm::SuperTypeVector*>(
      getExtendedSlot(js::FunctionExtended::WASM_STV_SLOT).toPrivate());
}

inline const js::wasm::TypeDef* JSFunction::wasmTypeDef() const {
  MOZ_ASSERT(isWasm());
  return wasmSuperTypeVector().typeDef();
}

namespace js {

JSString* FunctionToString(JSContext* cx, HandleFunction fun, bool isToSource);

/*
 * Report an error that call.thisv is not compatible with the specified class,
 * assuming that the method (clasp->name).prototype.<name of callee function>
 * is what was called.
 */
extern void ReportIncompatibleMethod(JSContext* cx, const CallArgs& args,
                                     const JSClass* clasp);

/*
 * Report an error that call.thisv is not an acceptable this for the callee
 * function.
 */
extern void ReportIncompatible(JSContext* cx, const CallArgs& args);

extern bool fun_apply(JSContext* cx, unsigned argc, Value* vp);

extern bool fun_call(JSContext* cx, unsigned argc, Value* vp);

} /* namespace js */

#ifdef DEBUG
namespace JS {
namespace detail {

JS_PUBLIC_API void CheckIsValidConstructible(const Value& calleev);

}  // namespace detail
}  // namespace JS
#endif

#endif /* vm_JSFunction_h */
