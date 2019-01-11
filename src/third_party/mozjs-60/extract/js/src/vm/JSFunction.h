/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSFunction_h
#define vm_JSFunction_h

/*
 * JS function definitions.
 */

#include "jstypes.h"

#include "vm/JSObject.h"
#include "vm/JSScript.h"

namespace js {

class FunctionExtended;

typedef JSNative           Native;
} // namespace js

struct JSAtomState;

static const uint32_t JSSLOT_BOUND_FUNCTION_TARGET     = 2;
static const uint32_t JSSLOT_BOUND_FUNCTION_THIS       = 3;
static const uint32_t JSSLOT_BOUND_FUNCTION_ARGS       = 4;

static const char FunctionConstructorMedialSigils[] = ") {\n";
static const char FunctionConstructorFinalBrace[] = "\n}";

enum class FunctionPrefixKind {
    None,
    Get,
    Set
};

class JSFunction : public js::NativeObject
{
  public:
    static const js::Class class_;

    enum FunctionKind {
        NormalFunction = 0,
        Arrow,                      /* ES6 '(args) => body' syntax */
        Method,                     /* ES6 MethodDefinition */
        ClassConstructor,
        Getter,
        Setter,
        AsmJS,                      /* function is an asm.js module or exported function */
        FunctionKindLimit
    };

    enum Flags {
        INTERPRETED      = 0x0001,  /* function has a JSScript and environment. */
        CONSTRUCTOR      = 0x0002,  /* function that can be called as a constructor */
        EXTENDED         = 0x0004,  /* structure is FunctionExtended */
        BOUND_FUN        = 0x0008,  /* function was created with Function.prototype.bind. */
        WASM_OPTIMIZED   = 0x0010,  /* asm.js/wasm function that has a jit entry */
        HAS_GUESSED_ATOM = 0x0020,  /* function had no explicit name, but a
                                       name was guessed for it anyway */
        HAS_BOUND_FUNCTION_NAME_PREFIX = 0x0020, /* bound functions reuse the HAS_GUESSED_ATOM
                                                    flag to track if atom_ already contains the
                                                    "bound " function name prefix */
        LAMBDA           = 0x0040,  /* function comes from a FunctionExpression, ArrowFunction, or
                                       Function() call (not a FunctionDeclaration or nonstandard
                                       function-statement) */
        SELF_HOSTED      = 0x0080,  /* function is self-hosted builtin and must not be
                                       decompilable nor constructible. */
        HAS_COMPILE_TIME_NAME = 0x0100, /* function had no explicit name, but a
                                           name was set by SetFunctionName
                                           at compile time */
        INTERPRETED_LAZY = 0x0200,  /* function is interpreted but doesn't have a script yet */
        RESOLVED_LENGTH  = 0x0400,  /* f.length has been resolved (see fun_resolve). */
        RESOLVED_NAME    = 0x0800,  /* f.name has been resolved (see fun_resolve). */

        FUNCTION_KIND_SHIFT = 13,
        FUNCTION_KIND_MASK  = 0x7 << FUNCTION_KIND_SHIFT,

        ASMJS_KIND = AsmJS << FUNCTION_KIND_SHIFT,
        ARROW_KIND = Arrow << FUNCTION_KIND_SHIFT,
        METHOD_KIND = Method << FUNCTION_KIND_SHIFT,
        CLASSCONSTRUCTOR_KIND = ClassConstructor << FUNCTION_KIND_SHIFT,
        GETTER_KIND = Getter << FUNCTION_KIND_SHIFT,
        SETTER_KIND = Setter << FUNCTION_KIND_SHIFT,

        /* Derived Flags values for convenience: */
        NATIVE_FUN = 0,
        NATIVE_CTOR = NATIVE_FUN | CONSTRUCTOR,
        NATIVE_CLASS_CTOR = NATIVE_FUN | CONSTRUCTOR | CLASSCONSTRUCTOR_KIND,
        ASMJS_CTOR = ASMJS_KIND | NATIVE_CTOR,
        ASMJS_LAMBDA_CTOR = ASMJS_KIND | NATIVE_CTOR | LAMBDA,
        WASM_FUN = NATIVE_FUN | WASM_OPTIMIZED,
        INTERPRETED_METHOD = INTERPRETED | METHOD_KIND,
        INTERPRETED_METHOD_GENERATOR_OR_ASYNC = INTERPRETED | METHOD_KIND,
        INTERPRETED_CLASS_CONSTRUCTOR = INTERPRETED | CLASSCONSTRUCTOR_KIND | CONSTRUCTOR,
        INTERPRETED_GETTER = INTERPRETED | GETTER_KIND,
        INTERPRETED_SETTER = INTERPRETED | SETTER_KIND,
        INTERPRETED_LAMBDA = INTERPRETED | LAMBDA | CONSTRUCTOR,
        INTERPRETED_LAMBDA_ARROW = INTERPRETED | LAMBDA | ARROW_KIND,
        INTERPRETED_LAMBDA_GENERATOR_OR_ASYNC = INTERPRETED | LAMBDA,
        INTERPRETED_NORMAL = INTERPRETED | CONSTRUCTOR,
        INTERPRETED_GENERATOR_OR_ASYNC = INTERPRETED,
        NO_XDR_FLAGS = RESOLVED_LENGTH | RESOLVED_NAME,

        STABLE_ACROSS_CLONES = CONSTRUCTOR | LAMBDA | SELF_HOSTED | HAS_COMPILE_TIME_NAME |
                               FUNCTION_KIND_MASK
    };

    static_assert((INTERPRETED | INTERPRETED_LAZY) == js::JS_FUNCTION_INTERPRETED_BITS,
                  "jsfriendapi.h's JSFunction::INTERPRETED-alike is wrong");
    static_assert(((FunctionKindLimit - 1) << FUNCTION_KIND_SHIFT) <= FUNCTION_KIND_MASK,
                  "FunctionKind doesn't fit into flags_");

  private:
    uint16_t        nargs_;       /* number of formal arguments
                                     (including defaults and the rest parameter unlike f.length) */
    uint16_t        flags_;       /* bitfield composed of the above Flags enum, as well as the kind */
    union U {
        class {
            friend class JSFunction;
            js::Native func_;          /* native method pointer or null */
            union {
                // Information about this function to be used by the JIT, only
                // used if isBuiltinNative(); use the accessor!
                const JSJitInfo* jitInfo_;
                // asm.js function index, only used if isAsmJSNative().
                size_t asmJSFuncIndex_;
                // for wasm, a pointer to a fast jit->wasm table entry.
                void** wasmJitEntry_;
            } extra;
        } native;
        struct {
            JSObject* env_;            /* environment for new activations */
            union {
                JSScript* script_;     /* interpreted bytecode descriptor or
                                          null; use the accessor! */
                js::LazyScript* lazy_; /* lazily compiled script, or nullptr */
            } s;
        } scripted;
    } u;
    js::GCPtrAtom atom_; /* name for diagnostics and decompiling */

  public:
    /* Call objects must be created for each invocation of this function. */
    bool needsCallObject() const {
        MOZ_ASSERT(!isInterpretedLazy());

        if (isNative())
            return false;

        // Note: this should be kept in sync with
        // FunctionBox::needsCallObjectRegardlessOfBindings().
        MOZ_ASSERT_IF(nonLazyScript()->funHasExtensibleScope() ||
                      nonLazyScript()->needsHomeObject()       ||
                      nonLazyScript()->isDerivedClassConstructor() ||
                      isGenerator() ||
                      isAsync(),
                      nonLazyScript()->bodyScope()->hasEnvironment());

        return nonLazyScript()->bodyScope()->hasEnvironment();
    }

    bool needsExtraBodyVarEnvironment() const;
    bool needsNamedLambdaEnvironment() const;

    bool needsFunctionEnvironmentObjects() const {
        return needsCallObject() || needsNamedLambdaEnvironment();
    }

    bool needsSomeEnvironmentObject() const {
        return needsFunctionEnvironmentObjects() || needsExtraBodyVarEnvironment();
    }

    size_t nargs() const {
        return nargs_;
    }

    uint16_t flags() const {
        return flags_;
    }

    FunctionKind kind() const {
        return static_cast<FunctionKind>((flags_ & FUNCTION_KIND_MASK) >> FUNCTION_KIND_SHIFT);
    }

    /* A function can be classified as either native (C++) or interpreted (JS): */
    bool isInterpreted()            const { return flags() & (INTERPRETED | INTERPRETED_LAZY); }
    bool isNative()                 const { return !isInterpreted(); }

    bool isConstructor()            const { return flags() & CONSTRUCTOR; }

    /* Possible attributes of a native function: */
    bool isAsmJSNative()            const { return kind() == AsmJS; }
    bool isWasmOptimized()          const { return (flags() & WASM_OPTIMIZED); }
    bool isBuiltinNative()          const { return isNativeWithCppEntry() && !isAsmJSNative(); }

    // May be called from the JIT with the wasmJitEntry_ field.
    bool isNativeWithJitEntry()     const { return isNative() && isWasmOptimized(); }
    // Must be called from the JIT with the native_ field.
    bool isNativeWithCppEntry()     const { return isNative() && !isWasmOptimized(); }

    /* Possible attributes of an interpreted function: */
    bool isBoundFunction()          const { return flags() & BOUND_FUN; }
    bool hasCompileTimeName()       const { return flags() & HAS_COMPILE_TIME_NAME; }
    bool hasGuessedAtom()           const {
        static_assert(HAS_GUESSED_ATOM == HAS_BOUND_FUNCTION_NAME_PREFIX,
                      "HAS_GUESSED_ATOM is unused for bound functions");
        return (flags() & (HAS_GUESSED_ATOM | BOUND_FUN)) == HAS_GUESSED_ATOM;
    }
    bool hasBoundFunctionNamePrefix() const {
        static_assert(HAS_BOUND_FUNCTION_NAME_PREFIX == HAS_GUESSED_ATOM,
                      "HAS_BOUND_FUNCTION_NAME_PREFIX is only used for bound functions");
        MOZ_ASSERT(isBoundFunction());
        return flags() & HAS_BOUND_FUNCTION_NAME_PREFIX;
    }
    bool isLambda()                 const { return flags() & LAMBDA; }
    bool isInterpretedLazy()        const { return flags() & INTERPRETED_LAZY; }
    bool hasScript()                const { return flags() & INTERPRETED; }

    bool infallibleIsDefaultClassConstructor(JSContext* cx) const;

    // Arrow functions store their lexical new.target in the first extended slot.
    bool isArrow()                  const { return kind() == Arrow; }
    // Every class-constructor is also a method.
    bool isMethod()                 const { return kind() == Method || kind() == ClassConstructor; }
    bool isClassConstructor()       const { return kind() == ClassConstructor; }

    bool isGetter()                 const { return kind() == Getter; }
    bool isSetter()                 const { return kind() == Setter; }

    bool allowSuperProperty() const {
        return isMethod() || isGetter() || isSetter();
    }

    bool hasResolvedLength()        const { return flags() & RESOLVED_LENGTH; }
    bool hasResolvedName()          const { return flags() & RESOLVED_NAME; }

    bool isSelfHostedOrIntrinsic()  const { return flags() & SELF_HOSTED; }
    bool isSelfHostedBuiltin()      const { return isSelfHostedOrIntrinsic() && !isNative(); }
    bool isIntrinsic()              const { return isSelfHostedOrIntrinsic() && isNative(); }

    bool hasJITCode() const {
        if (!hasScript())
            return false;

        return nonLazyScript()->hasBaselineScript() || nonLazyScript()->hasIonScript();
    }
    bool hasJitEntry() const {
        return hasScript() || isNativeWithJitEntry();
    }

    /* Compound attributes: */
    bool isBuiltin() const {
        return isBuiltinNative() || isNativeWithJitEntry() || isSelfHostedBuiltin();
    }

    bool isNamedLambda() const {
        return isLambda() && displayAtom() && !hasCompileTimeName() && !hasGuessedAtom();
    }

    bool hasLexicalThis() const {
        return isArrow();
    }

    bool isBuiltinFunctionConstructor();
    bool needsPrototypeProperty();

    /* Returns the strictness of this function, which must be interpreted. */
    bool strict() const {
        MOZ_ASSERT(isInterpreted());
        return isInterpretedLazy() ? lazyScript()->strict() : nonLazyScript()->strict();
    }

    void setFlags(uint16_t flags) {
        this->flags_ = flags;
    }
    void setKind(FunctionKind kind) {
        this->flags_ &= ~FUNCTION_KIND_MASK;
        this->flags_ |= static_cast<uint16_t>(kind) << FUNCTION_KIND_SHIFT;
    }

    // Make the function constructible.
    void setIsConstructor() {
        MOZ_ASSERT(!isConstructor());
        MOZ_ASSERT(isSelfHostedBuiltin());
        flags_ |= CONSTRUCTOR;
    }

    void setIsClassConstructor() {
        MOZ_ASSERT(!isClassConstructor());
        MOZ_ASSERT(isConstructor());

        setKind(ClassConstructor);
    }

    // Can be called multiple times by the parser.
    void setArgCount(uint16_t nargs) {
        this->nargs_ = nargs;
    }

    void setIsBoundFunction() {
        MOZ_ASSERT(!isBoundFunction());
        flags_ |= BOUND_FUN;
    }

    void setIsSelfHostedBuiltin() {
        MOZ_ASSERT(isInterpreted());
        MOZ_ASSERT(!isSelfHostedBuiltin());
        flags_ |= SELF_HOSTED;
        // Self-hosted functions should not be constructable.
        flags_ &= ~CONSTRUCTOR;
    }
    void setIsIntrinsic() {
        MOZ_ASSERT(isNative());
        MOZ_ASSERT(!isIntrinsic());
        flags_ |= SELF_HOSTED;
    }

    void setArrow() {
        setKind(Arrow);
    }

    void setResolvedLength() {
        flags_ |= RESOLVED_LENGTH;
    }

    void setResolvedName() {
        flags_ |= RESOLVED_NAME;
    }

    void setAsyncKind(js::FunctionAsyncKind asyncKind) {
        if (isInterpretedLazy())
            lazyScript()->setAsyncKind(asyncKind);
        else
            nonLazyScript()->setAsyncKind(asyncKind);
    }

    static bool getUnresolvedLength(JSContext* cx, js::HandleFunction fun,
                                    js::MutableHandleValue v);

    static bool getUnresolvedName(JSContext* cx, js::HandleFunction fun,
                                  js::MutableHandleAtom v);

    JSAtom* explicitName() const {
        return (hasCompileTimeName() || hasGuessedAtom()) ? nullptr : atom_.get();
    }
    JSAtom* explicitOrCompileTimeName() const {
        return hasGuessedAtom() ? nullptr : atom_.get();
    }

    void initAtom(JSAtom* atom) {
        MOZ_ASSERT_IF(atom, js::AtomIsMarked(zone(), atom));
        atom_.init(atom);
    }

    void setAtom(JSAtom* atom) {
        MOZ_ASSERT_IF(atom, js::AtomIsMarked(zone(), atom));
        atom_ = atom;
    }

    JSAtom* displayAtom() const {
        return atom_;
    }

    void setCompileTimeName(JSAtom* atom) {
        MOZ_ASSERT(!atom_);
        MOZ_ASSERT(atom);
        MOZ_ASSERT(!hasGuessedAtom());
        MOZ_ASSERT(!isClassConstructor());
        setAtom(atom);
        flags_ |= HAS_COMPILE_TIME_NAME;
    }
    JSAtom* compileTimeName() const {
        MOZ_ASSERT(hasCompileTimeName());
        MOZ_ASSERT(atom_);
        return atom_;
    }

    void setGuessedAtom(JSAtom* atom) {
        MOZ_ASSERT(!atom_);
        MOZ_ASSERT(atom);
        MOZ_ASSERT(!hasCompileTimeName());
        MOZ_ASSERT(!hasGuessedAtom());
        MOZ_ASSERT(!isBoundFunction());
        setAtom(atom);
        flags_ |= HAS_GUESSED_ATOM;
    }
    void clearGuessedAtom() {
        MOZ_ASSERT(hasGuessedAtom());
        MOZ_ASSERT(!isBoundFunction());
        MOZ_ASSERT(atom_);
        setAtom(nullptr);
        flags_ &= ~HAS_GUESSED_ATOM;
    }

    void setPrefixedBoundFunctionName(JSAtom* atom) {
        MOZ_ASSERT(!hasBoundFunctionNamePrefix());
        MOZ_ASSERT(atom);
        flags_ |= HAS_BOUND_FUNCTION_NAME_PREFIX;
        setAtom(atom);
    }

    /* uint16_t representation bounds number of call object dynamic slots. */
    enum { MAX_ARGS_AND_VARS = 2 * ((1U << 16) - 1) };

    /*
     * For an interpreted function, accessors for the initial scope object of
     * activations (stack frames) of the function.
     */
    JSObject* environment() const {
        MOZ_ASSERT(isInterpreted());
        return u.scripted.env_;
    }

    void setEnvironment(JSObject* obj) {
        MOZ_ASSERT(isInterpreted());
        *reinterpret_cast<js::GCPtrObject*>(&u.scripted.env_) = obj;
    }

    void initEnvironment(JSObject* obj) {
        MOZ_ASSERT(isInterpreted());
        reinterpret_cast<js::GCPtrObject*>(&u.scripted.env_)->init(obj);
    }

    void unsetEnvironment() {
        setEnvironment(nullptr);
    }

  public:
    static constexpr size_t offsetOfNargs() { return offsetof(JSFunction, nargs_); }
    static constexpr size_t offsetOfFlags() { return offsetof(JSFunction, flags_); }
    static size_t offsetOfEnvironment() { return offsetof(JSFunction, u.scripted.env_); }
    static size_t offsetOfAtom() { return offsetof(JSFunction, atom_); }

    static bool createScriptForLazilyInterpretedFunction(JSContext* cx, js::HandleFunction fun);
    void maybeRelazify(JSRuntime* rt);

    // Function Scripts
    //
    // Interpreted functions may either have an explicit JSScript (hasScript())
    // or be lazy with sufficient information to construct the JSScript if
    // necessary (isInterpretedLazy()).
    //
    // A lazy function will have a LazyScript if the function came from parsed
    // source, or nullptr if the function is a clone of a self hosted function.
    //
    // There are several methods to get the script of an interpreted function:
    //
    // - For all interpreted functions, getOrCreateScript() will get the
    //   JSScript, delazifying the function if necessary. This is the safest to
    //   use, but has extra checks, requires a cx and may trigger a GC.
    //
    // - For inlined functions which may have a LazyScript but whose JSScript
    //   is known to exist, existingScript() will get the script and delazify
    //   the function if necessary. If the function should not be delazified,
    //   use existingScriptNonDelazifying().
    //
    // - For functions known to have a JSScript, nonLazyScript() will get it.

    static JSScript* getOrCreateScript(JSContext* cx, js::HandleFunction fun) {
        MOZ_ASSERT(fun->isInterpreted());
        MOZ_ASSERT(cx);
        if (fun->isInterpretedLazy()) {
            if (!createScriptForLazilyInterpretedFunction(cx, fun))
                return nullptr;
            return fun->nonLazyScript();
        }
        return fun->nonLazyScript();
    }

    JSScript* existingScriptNonDelazifying() const {
        MOZ_ASSERT(isInterpreted());
        if (isInterpretedLazy()) {
            // Get the script from the canonical function. Ion used the
            // canonical function to inline the script and because it has
            // Baseline code it has not been relazified. Note that we can't
            // use lazyScript->script_ here as it may be null in some cases,
            // see bug 976536.
            js::LazyScript* lazy = lazyScript();
            JSFunction* fun = lazy->functionNonDelazifying();
            MOZ_ASSERT(fun);
            return fun->nonLazyScript();
        }
        return nonLazyScript();
    }

    JSScript* existingScript() {
        MOZ_ASSERT(isInterpreted());
        if (isInterpretedLazy()) {
            if (shadowZone()->needsIncrementalBarrier())
                js::LazyScript::writeBarrierPre(lazyScript());
            JSScript* script = existingScriptNonDelazifying();
            flags_ &= ~INTERPRETED_LAZY;
            flags_ |= INTERPRETED;
            initScript(script);
        }
        return nonLazyScript();
    }

    // The state of a JSFunction whose script errored out during bytecode
    // compilation. Such JSFunctions are only reachable via GC iteration and
    // not from script.
    bool hasUncompiledScript() const {
        MOZ_ASSERT(hasScript());
        return !u.scripted.s.script_;
    }

    JSScript* nonLazyScript() const {
        MOZ_ASSERT(!hasUncompiledScript());
        return u.scripted.s.script_;
    }

    static bool getLength(JSContext* cx, js::HandleFunction fun, uint16_t* length);

    js::LazyScript* lazyScript() const {
        MOZ_ASSERT(isInterpretedLazy() && u.scripted.s.lazy_);
        return u.scripted.s.lazy_;
    }

    js::LazyScript* lazyScriptOrNull() const {
        MOZ_ASSERT(isInterpretedLazy());
        return u.scripted.s.lazy_;
    }

    js::GeneratorKind generatorKind() const {
        if (!isInterpreted())
            return js::GeneratorKind::NotGenerator;
        if (hasScript())
            return nonLazyScript()->generatorKind();
        if (js::LazyScript* lazy = lazyScriptOrNull())
            return lazy->generatorKind();
        MOZ_ASSERT(isSelfHostedBuiltin());
        return js::GeneratorKind::NotGenerator;
    }

    bool isGenerator() const { return generatorKind() == js::GeneratorKind::Generator; }

    js::FunctionAsyncKind asyncKind() const {
        return isInterpretedLazy() ? lazyScript()->asyncKind() : nonLazyScript()->asyncKind();
    }

    bool isAsync() const {
        if (isInterpretedLazy())
            return lazyScript()->isAsync();
        if (hasScript())
            return nonLazyScript()->isAsync();
        return false;
    }

    void setScript(JSScript* script_) {
        mutableScript() = script_;
    }

    void initScript(JSScript* script_) {
        mutableScript().init(script_);
    }

    void setUnlazifiedScript(JSScript* script) {
        MOZ_ASSERT(isInterpretedLazy());
        if (lazyScriptOrNull()) {
            // Trigger a pre barrier on the lazy script being overwritten.
            js::LazyScript::writeBarrierPre(lazyScriptOrNull());
            if (!lazyScript()->maybeScript())
                lazyScript()->initScript(script);
        }
        flags_ &= ~INTERPRETED_LAZY;
        flags_ |= INTERPRETED;
        initScript(script);
    }

    void initLazyScript(js::LazyScript* lazy) {
        MOZ_ASSERT(isInterpreted());
        flags_ &= ~INTERPRETED;
        flags_ |= INTERPRETED_LAZY;
        u.scripted.s.lazy_ = lazy;
    }

    JSNative native() const {
        MOZ_ASSERT(isNative());
        return u.native.func_;
    }

    JSNative maybeNative() const {
        return isInterpreted() ? nullptr : native();
    }

    void initNative(js::Native native, const JSJitInfo* jitInfo) {
        MOZ_ASSERT(isNativeWithCppEntry());
        MOZ_ASSERT_IF(jitInfo, isBuiltinNative());
        MOZ_ASSERT(native);
        u.native.func_ = native;
        u.native.extra.jitInfo_ = jitInfo;
    }
    bool hasJitInfo() const {
        return isBuiltinNative() && u.native.extra.jitInfo_;
    }
    const JSJitInfo* jitInfo() const {
        MOZ_ASSERT(hasJitInfo());
        return u.native.extra.jitInfo_;
    }
    void setJitInfo(const JSJitInfo* data) {
        MOZ_ASSERT(isBuiltinNative());
        u.native.extra.jitInfo_ = data;
    }

    // Wasm natives are optimized and have a jit entry.
    void initWasmNative(js::Native native) {
        MOZ_ASSERT(isNativeWithJitEntry());
        MOZ_ASSERT(native);
        u.native.func_ = native;
        u.native.extra.wasmJitEntry_ = nullptr;
    }
    void setWasmJitEntry(void** entry) {
        MOZ_ASSERT(isNativeWithJitEntry());
        MOZ_ASSERT(entry);
        MOZ_ASSERT(!u.native.extra.wasmJitEntry_);
        u.native.extra.wasmJitEntry_ = entry;
    }
    void** wasmJitEntry() const {
        MOZ_ASSERT(isNativeWithJitEntry());
        MOZ_ASSERT(u.native.extra.wasmJitEntry_);
        return u.native.extra.wasmJitEntry_;
    }

    // AsmJS functions store the func index in the jitinfo slot, since these
    // don't have a jit info associated.
    void setAsmJSIndex(uint32_t funcIndex) {
        MOZ_ASSERT(isAsmJSNative());
        MOZ_ASSERT(!isWasmOptimized());
        MOZ_ASSERT(!u.native.extra.asmJSFuncIndex_);
        u.native.extra.asmJSFuncIndex_ = funcIndex;
    }
    uint32_t asmJSFuncIndex() const {
        MOZ_ASSERT(isAsmJSNative());
        MOZ_ASSERT(!isWasmOptimized());
        return u.native.extra.asmJSFuncIndex_;
    }

    bool isDerivedClassConstructor();

    static unsigned offsetOfNative() {
        return offsetof(JSFunction, u.native.func_);
    }
    static unsigned offsetOfScript() {
        static_assert(offsetof(U, scripted.s.script_) == offsetof(U, native.extra.wasmJitEntry_),
                      "scripted.s.script_ must be at the same offset as native.extra.wasmJitEntry_");
        return offsetof(JSFunction, u.scripted.s.script_);
    }
    static unsigned offsetOfNativeOrEnv() {
        static_assert(offsetof(U, native.func_) == offsetof(U, scripted.env_),
                      "U.native.func_ must be at the same offset as U.scripted.env_");
        return offsetOfNative();
    }
    static unsigned offsetOfScriptOrLazyScript() {
        static_assert(offsetof(U, scripted.s.script_) == offsetof(U, scripted.s.lazy_),
                      "U.scripted.s.script_ must be at the same offset as lazy_");
        return offsetof(JSFunction, u.scripted.s.script_);
    }

    static unsigned offsetOfJitInfo() {
        return offsetof(JSFunction, u.native.extra.jitInfo_);
    }

    inline void trace(JSTracer* trc);

    /* Bound function accessors. */

    JSObject* getBoundFunctionTarget() const;
    const js::Value& getBoundFunctionThis() const;
    const js::Value& getBoundFunctionArgument(unsigned which) const;
    size_t getBoundFunctionArgumentCount() const;

    /*
     * Used to mark bound functions as such and make them constructible if the
     * target is. Also assigns the prototype and sets the name and correct length.
     */
    static bool finishBoundFunctionInit(JSContext* cx, js::HandleFunction bound,
                                        js::HandleObject targetObj, int32_t argCount);

  private:
    js::GCPtrScript& mutableScript() {
        MOZ_ASSERT(hasScript());
        return *(js::GCPtrScript*)&u.scripted.s.script_;
    }

    inline js::FunctionExtended* toExtended();
    inline const js::FunctionExtended* toExtended() const;

  public:
    inline bool isExtended() const {
        bool extended = !!(flags() & EXTENDED);
        MOZ_ASSERT_IF(isTenured(),
                      extended == (asTenured().getAllocKind() == js::gc::AllocKind::FUNCTION_EXTENDED));
        return extended;
    }

    /*
     * Accessors for data stored in extended functions. Use setExtendedSlot if
     * the function has already been initialized. Otherwise use
     * initExtendedSlot.
     */
    inline void initializeExtended();
    inline void initExtendedSlot(size_t which, const js::Value& val);
    inline void setExtendedSlot(size_t which, const js::Value& val);
    inline const js::Value& getExtendedSlot(size_t which) const;

    /* Constructs a new type for the function if necessary. */
    static bool setTypeForScriptedFunction(JSContext* cx, js::HandleFunction fun,
                                           bool singleton = false);

    /* GC support. */
    js::gc::AllocKind getAllocKind() const {
        static_assert(js::gc::AllocKind::FUNCTION != js::gc::AllocKind::FUNCTION_EXTENDED,
                      "extended/non-extended AllocKinds have to be different "
                      "for getAllocKind() to have a reason to exist");

        js::gc::AllocKind kind = js::gc::AllocKind::FUNCTION;
        if (isExtended())
            kind = js::gc::AllocKind::FUNCTION_EXTENDED;
        MOZ_ASSERT_IF(isTenured(), kind == asTenured().getAllocKind());
        return kind;
    }
};

static_assert(sizeof(JSFunction) == sizeof(js::shadow::Function),
              "shadow interface must match actual interface");

extern JSString*
fun_toStringHelper(JSContext* cx, js::HandleObject obj, bool isToSource);

namespace js {

extern bool
Function(JSContext* cx, unsigned argc, Value* vp);

extern bool
Generator(JSContext* cx, unsigned argc, Value* vp);

extern bool
AsyncFunctionConstructor(JSContext* cx, unsigned argc, Value* vp);

extern bool
AsyncGeneratorConstructor(JSContext* cx, unsigned argc, Value* vp);

// If enclosingEnv is null, the function will have a null environment()
// (yes, null, not the global).  In all cases, the global will be used as the
// parent.

extern JSFunction*
NewFunctionWithProto(JSContext* cx, JSNative native, unsigned nargs,
                     JSFunction::Flags flags, HandleObject enclosingEnv, HandleAtom atom,
                     HandleObject proto, gc::AllocKind allocKind = gc::AllocKind::FUNCTION,
                     NewObjectKind newKind = GenericObject);

// Allocate a new function backed by a JSNative.  Note that by default this
// creates a singleton object.
inline JSFunction*
NewNativeFunction(JSContext* cx, JSNative native, unsigned nargs, HandleAtom atom,
                  gc::AllocKind allocKind = gc::AllocKind::FUNCTION,
                  NewObjectKind newKind = SingletonObject,
                  JSFunction::Flags flags = JSFunction::NATIVE_FUN)
{
    MOZ_ASSERT(native);
    return NewFunctionWithProto(cx, native, nargs, flags, nullptr, atom, nullptr, allocKind,
                                newKind);
}

// Allocate a new constructor backed by a JSNative.  Note that by default this
// creates a singleton object.
inline JSFunction*
NewNativeConstructor(JSContext* cx, JSNative native, unsigned nargs, HandleAtom atom,
                     gc::AllocKind allocKind = gc::AllocKind::FUNCTION,
                     NewObjectKind newKind = SingletonObject,
                     JSFunction::Flags flags = JSFunction::NATIVE_CTOR)
{
    MOZ_ASSERT(native);
    MOZ_ASSERT(flags & JSFunction::NATIVE_CTOR);
    return NewFunctionWithProto(cx, native, nargs, flags, nullptr, atom, nullptr, allocKind,
                                newKind);
}

// Allocate a new scripted function.  If enclosingEnv is null, the
// global will be used.  In all cases the parent of the resulting object will be
// the global.
extern JSFunction*
NewScriptedFunction(JSContext* cx, unsigned nargs, JSFunction::Flags flags,
                    HandleAtom atom, HandleObject proto = nullptr,
                    gc::AllocKind allocKind = gc::AllocKind::FUNCTION,
                    NewObjectKind newKind = GenericObject,
                    HandleObject enclosingEnv = nullptr);
extern JSAtom*
IdToFunctionName(JSContext* cx, HandleId id,
                 FunctionPrefixKind prefixKind = FunctionPrefixKind::None);

extern JSAtom*
NameToFunctionName(JSContext* cx, HandleAtom name,
                   FunctionPrefixKind prefixKind = FunctionPrefixKind::None);

extern bool
SetFunctionNameIfNoOwnName(JSContext* cx, HandleFunction fun, HandleValue name,
                           FunctionPrefixKind prefixKind);

extern JSFunction*
DefineFunction(JSContext* cx, HandleObject obj, HandleId id, JSNative native,
               unsigned nargs, unsigned flags,
               gc::AllocKind allocKind = gc::AllocKind::FUNCTION);

extern bool
fun_toString(JSContext* cx, unsigned argc, Value* vp);

struct WellKnownSymbols;

extern bool
FunctionHasDefaultHasInstance(JSFunction* fun, const WellKnownSymbols& symbols);

extern bool
fun_symbolHasInstance(JSContext* cx, unsigned argc, Value* vp);

extern void
ThrowTypeErrorBehavior(JSContext* cx);

/*
 * Function extended with reserved slots for use by various kinds of functions.
 * Most functions do not have these extensions, but enough do that efficient
 * storage is required (no malloc'ed reserved slots).
 */
class FunctionExtended : public JSFunction
{
  public:
    static const unsigned NUM_EXTENDED_SLOTS = 2;

    /* Arrow functions store their lexical new.target in the first extended slot. */
    static const unsigned ARROW_NEWTARGET_SLOT = 0;

    static const unsigned METHOD_HOMEOBJECT_SLOT = 0;

    /*
     * Exported asm.js/wasm functions store their WasmInstanceObject in the
     * first slot.
     */
    static const unsigned WASM_INSTANCE_SLOT = 0;

    /*
     * wasm/asm.js exported functions store the wasm::TlsData pointer of their
     * instance.
     */
    static const unsigned WASM_TLSDATA_SLOT = 1;

    /*
     * asm.js module functions store their WasmModuleObject in the first slot.
     */
    static const unsigned ASMJS_MODULE_SLOT = 0;


    static inline size_t offsetOfExtendedSlot(unsigned which) {
        MOZ_ASSERT(which < NUM_EXTENDED_SLOTS);
        return offsetof(FunctionExtended, extendedSlots) + which * sizeof(GCPtrValue);
    }
    static inline size_t offsetOfArrowNewTargetSlot() {
        return offsetOfExtendedSlot(ARROW_NEWTARGET_SLOT);
    }
    static inline size_t offsetOfMethodHomeObjectSlot() {
        return offsetOfExtendedSlot(METHOD_HOMEOBJECT_SLOT);
    }

  private:
    friend class JSFunction;

    /* Reserved slots available for storage by particular native functions. */
    GCPtrValue extendedSlots[NUM_EXTENDED_SLOTS];
};

extern bool
CanReuseScriptForClone(JSCompartment* compartment, HandleFunction fun, HandleObject newParent);

extern JSFunction*
CloneFunctionReuseScript(JSContext* cx, HandleFunction fun, HandleObject parent,
                         gc::AllocKind kind = gc::AllocKind::FUNCTION,
                         NewObjectKind newKindArg = GenericObject,
                         HandleObject proto = nullptr);

// Functions whose scripts are cloned are always given singleton types.
extern JSFunction*
CloneFunctionAndScript(JSContext* cx, HandleFunction fun, HandleObject parent,
                       HandleScope newScope,
                       gc::AllocKind kind = gc::AllocKind::FUNCTION,
                       HandleObject proto = nullptr);

extern JSFunction*
CloneAsmJSModuleFunction(JSContext* cx, HandleFunction fun);

extern JSFunction*
CloneSelfHostingIntrinsic(JSContext* cx, HandleFunction fun);

} // namespace js

inline js::FunctionExtended*
JSFunction::toExtended()
{
    MOZ_ASSERT(isExtended());
    return static_cast<js::FunctionExtended*>(this);
}

inline const js::FunctionExtended*
JSFunction::toExtended() const
{
    MOZ_ASSERT(isExtended());
    return static_cast<const js::FunctionExtended*>(this);
}

inline void
JSFunction::initializeExtended()
{
    MOZ_ASSERT(isExtended());

    MOZ_ASSERT(mozilla::ArrayLength(toExtended()->extendedSlots) == 2);
    toExtended()->extendedSlots[0].init(js::UndefinedValue());
    toExtended()->extendedSlots[1].init(js::UndefinedValue());
}

inline void
JSFunction::initExtendedSlot(size_t which, const js::Value& val)
{
    MOZ_ASSERT(which < mozilla::ArrayLength(toExtended()->extendedSlots));
    MOZ_ASSERT(js::IsObjectValueInCompartment(val, compartment()));
    toExtended()->extendedSlots[which].init(val);
}

inline void
JSFunction::setExtendedSlot(size_t which, const js::Value& val)
{
    MOZ_ASSERT(which < mozilla::ArrayLength(toExtended()->extendedSlots));
    MOZ_ASSERT(js::IsObjectValueInCompartment(val, compartment()));
    toExtended()->extendedSlots[which] = val;
}

inline const js::Value&
JSFunction::getExtendedSlot(size_t which) const
{
    MOZ_ASSERT(which < mozilla::ArrayLength(toExtended()->extendedSlots));
    return toExtended()->extendedSlots[which];
}

namespace js {

JSString* FunctionToString(JSContext* cx, HandleFunction fun, bool isToSource);

template<XDRMode mode>
bool
XDRInterpretedFunction(XDRState<mode>* xdr, HandleScope enclosingScope,
                       HandleScriptSource sourceObject, MutableHandleFunction objp);

/*
 * Report an error that call.thisv is not compatible with the specified class,
 * assuming that the method (clasp->name).prototype.<name of callee function>
 * is what was called.
 */
extern void
ReportIncompatibleMethod(JSContext* cx, const CallArgs& args, const Class* clasp);

/*
 * Report an error that call.thisv is not an acceptable this for the callee
 * function.
 */
extern void
ReportIncompatible(JSContext* cx, const CallArgs& args);

extern const JSFunctionSpec function_methods[];
extern const JSFunctionSpec function_selfhosted_methods[];

extern bool
fun_apply(JSContext* cx, unsigned argc, Value* vp);

extern bool
fun_call(JSContext* cx, unsigned argc, Value* vp);

} /* namespace js */

#ifdef DEBUG
namespace JS {
namespace detail {

JS_PUBLIC_API(void)
CheckIsValidConstructible(const Value& calleev);

} // namespace detail
} // namespace JS
#endif

#endif /* vm_JSFunction_h */
