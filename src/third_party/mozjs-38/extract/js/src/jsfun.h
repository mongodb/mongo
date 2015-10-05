/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsfun_h
#define jsfun_h

/*
 * JS function definitions.
 */

#include "jsobj.h"
#include "jsscript.h"
#include "jstypes.h"

namespace js {
class FunctionExtended;

typedef JSNative           Native;
}

struct JSAtomState;

class JSFunction : public js::NativeObject
{
  public:
    static const js::Class class_;

    enum Flags {
        INTERPRETED      = 0x0001,  /* function has a JSScript and environment. */
        NATIVE_CTOR      = 0x0002,  /* native that can be called as a constructor */
        EXTENDED         = 0x0004,  /* structure is FunctionExtended */
        IS_FUN_PROTO     = 0x0010,  /* function is Function.prototype for some global object */
        EXPR_CLOSURE     = 0x0020,  /* expression closure: function(x) x*x */
        HAS_GUESSED_ATOM = 0x0040,  /* function had no explicit name, but a
                                       name was guessed for it anyway */
        LAMBDA           = 0x0080,  /* function comes from a FunctionExpression, ArrowFunction, or
                                       Function() call (not a FunctionDeclaration or nonstandard
                                       function-statement) */
        SELF_HOSTED      = 0x0100,  /* function is self-hosted builtin and must not be
                                       decompilable nor constructible. */
        SELF_HOSTED_CTOR = 0x0200,  /* function is self-hosted builtin constructor and
                                       must be constructible but not decompilable. */
        HAS_REST         = 0x0400,  /* function has a rest (...) parameter */
        ASMJS            = 0x0800,  /* function is an asm.js module or exported function */
        INTERPRETED_LAZY = 0x1000,  /* function is interpreted but doesn't have a script yet */
        ARROW            = 0x2000,  /* ES6 '(args) => body' syntax */
        RESOLVED_LENGTH  = 0x4000,  /* f.length has been resolved (see js::fun_resolve). */
        RESOLVED_NAME    = 0x8000,  /* f.name has been resolved (see js::fun_resolve). */

        /* Derived Flags values for convenience: */
        NATIVE_FUN = 0,
        ASMJS_CTOR = ASMJS | NATIVE_CTOR,
        ASMJS_LAMBDA_CTOR = ASMJS | NATIVE_CTOR | LAMBDA,
        INTERPRETED_LAMBDA = INTERPRETED | LAMBDA,
        INTERPRETED_LAMBDA_ARROW = INTERPRETED | LAMBDA | ARROW,
        STABLE_ACROSS_CLONES = NATIVE_CTOR | IS_FUN_PROTO | EXPR_CLOSURE | HAS_GUESSED_ATOM |
                               LAMBDA | SELF_HOSTED | SELF_HOSTED_CTOR | HAS_REST | ASMJS | ARROW
    };

    static_assert(INTERPRETED == JS_FUNCTION_INTERPRETED_BIT,
                  "jsfriendapi.h's JSFunction::INTERPRETED-alike is wrong");

  private:
    uint16_t        nargs_;       /* number of formal arguments
                                     (including defaults and the rest parameter unlike f.length) */
    uint16_t        flags_;       /* bitfield composed of the above Flags enum */
    union U {
        class Native {
            friend class JSFunction;
            js::Native          native;       /* native method pointer or null */
            const JSJitInfo*    jitinfo;     /* Information about this function to be
                                                 used by the JIT;
                                                 use the accessor! */
        } n;
        struct Scripted {
            union {
                JSScript* script_; /* interpreted bytecode descriptor or null;
                                      use the accessor! */
                js::LazyScript* lazy_; /* lazily compiled script, or nullptr */
            } s;
            JSObject*   env_;    /* environment for new activations;
                                     use the accessor! */
        } i;
        void*           nativeOrScript;
    } u;
    js::HeapPtrAtom  atom_;       /* name for diagnostics and decompiling */

  public:

    /* Call objects must be created for each invocation of a heavyweight function. */
    bool isHeavyweight() const {
        MOZ_ASSERT(!isInterpretedLazy());

        if (isNative())
            return false;

        // Note: this should be kept in sync with FunctionBox::isHeavyweight().
        return nonLazyScript()->hasAnyAliasedBindings() ||
               nonLazyScript()->funHasExtensibleScope() ||
               nonLazyScript()->funNeedsDeclEnvObject() ||
               isGenerator();
    }

    size_t nargs() const {
        return nargs_;
    }

    uint16_t flags() const {
        return flags_;
    }

    /* A function can be classified as either native (C++) or interpreted (JS): */
    bool isInterpreted()            const { return flags() & (INTERPRETED | INTERPRETED_LAZY); }
    bool isNative()                 const { return !isInterpreted(); }

    /* Possible attributes of a native function: */
    bool isNativeConstructor()      const { return flags() & NATIVE_CTOR; }
    bool isAsmJSNative()            const { return flags() & ASMJS; }

    /* Possible attributes of an interpreted function: */
    bool isFunctionPrototype()      const { return flags() & IS_FUN_PROTO; }
    bool isExprClosure()            const { return flags() & EXPR_CLOSURE; }
    bool hasGuessedAtom()           const { return flags() & HAS_GUESSED_ATOM; }
    bool isLambda()                 const { return flags() & LAMBDA; }
    bool isSelfHostedBuiltin()      const { return flags() & SELF_HOSTED; }
    bool isSelfHostedConstructor()  const { return flags() & SELF_HOSTED_CTOR; }
    bool hasRest()                  const { return flags() & HAS_REST; }
    bool isInterpretedLazy()        const { return flags() & INTERPRETED_LAZY; }
    bool hasScript()                const { return flags() & INTERPRETED; }

    // Arrow functions store their lexical |this| in the first extended slot.
    bool isArrow()                  const { return flags() & ARROW; }

    bool hasResolvedLength()        const { return flags() & RESOLVED_LENGTH; }
    bool hasResolvedName()          const { return flags() & RESOLVED_NAME; }

    bool hasJITCode() const {
        if (!hasScript())
            return false;

        return nonLazyScript()->hasBaselineScript() || nonLazyScript()->hasIonScript();
    }

    /* Compound attributes: */
    bool isBuiltin() const {
        return (isNative() && !isAsmJSNative()) || isSelfHostedBuiltin();
    }
    bool isInterpretedConstructor() const {
        // Note: the JITs inline this check, so be careful when making changes
        // here. See MacroAssembler::branchIfNotInterpretedConstructor.
        return isInterpreted() && !isFunctionPrototype() && !isArrow() &&
               (!isSelfHostedBuiltin() || isSelfHostedConstructor());
    }
    bool isNamedLambda() const {
        return isLambda() && displayAtom() && !hasGuessedAtom();
    }

    bool isBuiltinFunctionConstructor();

    /* Returns the strictness of this function, which must be interpreted. */
    bool strict() const {
        MOZ_ASSERT(isInterpreted());
        return isInterpretedLazy() ? lazyScript()->strict() : nonLazyScript()->strict();
    }

    void setFlags(uint16_t flags) {
        this->flags_ = flags;
    }

    // Can be called multiple times by the parser.
    void setArgCount(uint16_t nargs) {
        this->nargs_ = nargs;
    }

    // Can be called multiple times by the parser.
    void setHasRest() {
        flags_ |= HAS_REST;
    }

    void setIsSelfHostedBuiltin() {
        MOZ_ASSERT(!isSelfHostedBuiltin());
        flags_ |= SELF_HOSTED;
    }

    void setIsSelfHostedConstructor() {
        MOZ_ASSERT(!isSelfHostedConstructor());
        flags_ |= SELF_HOSTED_CTOR;
    }

    void setIsFunctionPrototype() {
        MOZ_ASSERT(!isFunctionPrototype());
        flags_ |= IS_FUN_PROTO;
    }

    // Can be called multiple times by the parser.
    void setIsExprClosure() {
        flags_ |= EXPR_CLOSURE;
    }

    void setArrow() {
        flags_ |= ARROW;
    }

    void setResolvedLength() {
        flags_ |= RESOLVED_LENGTH;
    }

    void setResolvedName() {
        flags_ |= RESOLVED_NAME;
    }

    JSAtom* atom() const { return hasGuessedAtom() ? nullptr : atom_.get(); }

    js::PropertyName* name() const {
        return hasGuessedAtom() || !atom_ ? nullptr : atom_->asPropertyName();
    }

    void initAtom(JSAtom* atom) { atom_.init(atom); }

    JSAtom* displayAtom() const {
        return atom_;
    }

    void setGuessedAtom(JSAtom* atom) {
        MOZ_ASSERT(!atom_);
        MOZ_ASSERT(atom);
        MOZ_ASSERT(!hasGuessedAtom());
        atom_ = atom;
        flags_ |= HAS_GUESSED_ATOM;
    }

    /* uint16_t representation bounds number of call object dynamic slots. */
    enum { MAX_ARGS_AND_VARS = 2 * ((1U << 16) - 1) };

    /*
     * For an interpreted function, accessors for the initial scope object of
     * activations (stack frames) of the function.
     */
    JSObject* environment() const {
        MOZ_ASSERT(isInterpreted());
        return u.i.env_;
    }

    void setEnvironment(JSObject* obj) {
        MOZ_ASSERT(isInterpreted());
        *(js::HeapPtrObject*)&u.i.env_ = obj;
    }

    void initEnvironment(JSObject* obj) {
        MOZ_ASSERT(isInterpreted());
        ((js::HeapPtrObject*)&u.i.env_)->init(obj);
    }

    static inline size_t offsetOfNargs() { return offsetof(JSFunction, nargs_); }
    static inline size_t offsetOfFlags() { return offsetof(JSFunction, flags_); }
    static inline size_t offsetOfEnvironment() { return offsetof(JSFunction, u.i.env_); }
    static inline size_t offsetOfAtom() { return offsetof(JSFunction, atom_); }

    static bool createScriptForLazilyInterpretedFunction(JSContext* cx, js::HandleFunction fun);
    void relazify(JSTracer* trc);

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
    //   is known to exist, existingScriptForInlinedFunction() will get the
    //   script and delazify the function if necessary.
    //
    // - For functions known to have a JSScript, nonLazyScript() will get it.

    JSScript* getOrCreateScript(JSContext* cx) {
        MOZ_ASSERT(isInterpreted());
        MOZ_ASSERT(cx);
        if (isInterpretedLazy()) {
            JS::RootedFunction self(cx, this);
            if (!createScriptForLazilyInterpretedFunction(cx, self))
                return nullptr;
            return self->nonLazyScript();
        }
        return nonLazyScript();
    }

    JSScript* existingScriptForInlinedFunction() {
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
            JSScript* script = fun->nonLazyScript();

            if (shadowZone()->needsIncrementalBarrier())
                js::LazyScript::writeBarrierPre(lazy);

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
        return !u.i.s.script_;
    }

    JSScript* nonLazyScript() const {
        MOZ_ASSERT(!hasUncompiledScript());
        return u.i.s.script_;
    }

    bool getLength(JSContext* cx, uint16_t* length) {
        JS::RootedFunction self(cx, this);
        if (self->isInterpretedLazy() && !self->getOrCreateScript(cx))
            return false;

        *length = self->hasScript() ? self->nonLazyScript()->funLength()
                                    : (self->nargs() - self->hasRest());
        return true;
    }

    js::HeapPtrScript& mutableScript() {
        MOZ_ASSERT(isInterpreted());
        return *(js::HeapPtrScript*)&u.i.s.script_;
    }

    js::LazyScript* lazyScript() const {
        MOZ_ASSERT(isInterpretedLazy() && u.i.s.lazy_);
        return u.i.s.lazy_;
    }

    js::LazyScript* lazyScriptOrNull() const {
        MOZ_ASSERT(isInterpretedLazy());
        return u.i.s.lazy_;
    }

    js::GeneratorKind generatorKind() const {
        if (!isInterpreted())
            return js::NotGenerator;
        if (hasScript())
            return nonLazyScript()->generatorKind();
        if (js::LazyScript* lazy = lazyScriptOrNull())
            return lazy->generatorKind();
        MOZ_ASSERT(isSelfHostedBuiltin());
        return js::NotGenerator;
    }

    bool isGenerator() const { return generatorKind() != js::NotGenerator; }

    bool isLegacyGenerator() const { return generatorKind() == js::LegacyGenerator; }

    bool isStarGenerator() const { return generatorKind() == js::StarGenerator; }

    void setScript(JSScript* script_) {
        MOZ_ASSERT(hasScript());
        mutableScript() = script_;
    }

    void initScript(JSScript* script_) {
        MOZ_ASSERT(hasScript());
        mutableScript().init(script_);
    }

    void setUnlazifiedScript(JSScript* script) {
        // Note: createScriptForLazilyInterpretedFunction triggers a barrier on
        // lazy script before it is overwritten here.
        MOZ_ASSERT(isInterpretedLazy());
        if (!lazyScript()->maybeScript())
            lazyScript()->initScript(script);
        flags_ &= ~INTERPRETED_LAZY;
        flags_ |= INTERPRETED;
        initScript(script);
    }

    void initLazyScript(js::LazyScript* lazy) {
        MOZ_ASSERT(isInterpreted());
        flags_ &= ~INTERPRETED;
        flags_ |= INTERPRETED_LAZY;
        u.i.s.lazy_ = lazy;
    }

    JSNative native() const {
        MOZ_ASSERT(isNative());
        return u.n.native;
    }

    JSNative maybeNative() const {
        return isInterpreted() ? nullptr : native();
    }

    void initNative(js::Native native, const JSJitInfo* jitinfo) {
        MOZ_ASSERT(native);
        u.n.native = native;
        u.n.jitinfo = jitinfo;
    }

    const JSJitInfo* jitInfo() const {
        MOZ_ASSERT(isNative());
        return u.n.jitinfo;
    }

    void setJitInfo(const JSJitInfo* data) {
        MOZ_ASSERT(isNative());
        u.n.jitinfo = data;
    }

    static unsigned offsetOfNativeOrScript() {
        static_assert(offsetof(U, n.native) == offsetof(U, i.s.script_),
                      "native and script pointers must be in the same spot "
                      "for offsetOfNativeOrScript() have any sense");
        static_assert(offsetof(U, n.native) == offsetof(U, nativeOrScript),
                      "U::nativeOrScript must be at the same offset as "
                      "native");

        return offsetof(JSFunction, u.nativeOrScript);
    }

#if JS_BITS_PER_WORD == 32
    static const js::gc::AllocKind FinalizeKind = js::gc::FINALIZE_OBJECT2_BACKGROUND;
    static const js::gc::AllocKind ExtendedFinalizeKind = js::gc::FINALIZE_OBJECT4_BACKGROUND;
#else
    static const js::gc::AllocKind FinalizeKind = js::gc::FINALIZE_OBJECT4_BACKGROUND;
    static const js::gc::AllocKind ExtendedFinalizeKind = js::gc::FINALIZE_OBJECT8_BACKGROUND;
#endif

    inline void trace(JSTracer* trc);

    /* Bound function accessors. */

    inline bool initBoundFunction(JSContext* cx, js::HandleObject target, js::HandleValue thisArg,
                                  const js::Value* args, unsigned argslen);

    JSObject* getBoundFunctionTarget() const;
    const js::Value& getBoundFunctionThis() const;
    const js::Value& getBoundFunctionArgument(unsigned which) const;
    size_t getBoundFunctionArgumentCount() const;

  private:
    inline js::FunctionExtended* toExtended();
    inline const js::FunctionExtended* toExtended() const;

  public:
    inline bool isExtended() const {
        MOZ_ASSERT_IF(isTenured(), !!(flags() & EXTENDED) == (asTenured().getAllocKind() == ExtendedFinalizeKind));
        return !!(flags() & EXTENDED);
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
    static bool setTypeForScriptedFunction(js::ExclusiveContext* cx, js::HandleFunction fun,
                                           bool singleton = false);

    /* GC support. */
    js::gc::AllocKind getAllocKind() const {
        static_assert(FinalizeKind != ExtendedFinalizeKind,
                      "extended/non-extended AllocKinds have to be different "
                      "for getAllocKind() to have a reason to exist");

        js::gc::AllocKind kind = FinalizeKind;
        if (isExtended())
            kind = ExtendedFinalizeKind;
        MOZ_ASSERT_IF(isTenured(), kind == asTenured().getAllocKind());
        return kind;
    }
};

static_assert(sizeof(JSFunction) == sizeof(js::shadow::Function),
              "shadow interface must match actual interface");

extern JSString*
fun_toStringHelper(JSContext* cx, js::HandleObject obj, unsigned indent);

inline JSFunction::Flags
JSAPIToJSFunctionFlags(unsigned flags)
{
    return (flags & JSFUN_CONSTRUCTOR)
           ? JSFunction::NATIVE_CTOR
           : JSFunction::NATIVE_FUN;
}

namespace js {

extern bool
Function(JSContext* cx, unsigned argc, Value* vp);

extern bool
Generator(JSContext* cx, unsigned argc, Value* vp);

extern JSFunction*
NewFunction(ExclusiveContext* cx, HandleObject funobj, JSNative native, unsigned nargs,
            JSFunction::Flags flags, HandleObject parent, HandleAtom atom,
            gc::AllocKind allocKind = JSFunction::FinalizeKind,
            NewObjectKind newKind = GenericObject);

// If proto is nullptr, Function.prototype is used instead.
extern JSFunction*
NewFunctionWithProto(ExclusiveContext* cx, HandleObject funobj, JSNative native, unsigned nargs,
                     JSFunction::Flags flags, HandleObject parent, HandleAtom atom,
                     HandleObject proto, gc::AllocKind allocKind = JSFunction::FinalizeKind,
                     NewObjectKind newKind = GenericObject);

extern JSAtom*
IdToFunctionName(JSContext* cx, HandleId id);

extern JSFunction*
DefineFunction(JSContext* cx, HandleObject obj, HandleId id, JSNative native,
               unsigned nargs, unsigned flags,
               gc::AllocKind allocKind = JSFunction::FinalizeKind,
               NewObjectKind newKind = GenericObject);

bool
FunctionHasResolveHook(const JSAtomState& atomState, jsid id);

extern bool
fun_resolve(JSContext* cx, HandleObject obj, HandleId id, bool* resolvedp);

extern bool
fun_toString(JSContext* cx, unsigned argc, Value* vp);

extern bool
fun_bind(JSContext* cx, unsigned argc, Value* vp);

/*
 * Function extended with reserved slots for use by various kinds of functions.
 * Most functions do not have these extensions, but enough do that efficient
 * storage is required (no malloc'ed reserved slots).
 */
class FunctionExtended : public JSFunction
{
  public:
    static const unsigned NUM_EXTENDED_SLOTS = 2;

    /* Arrow functions store their lexical |this| in the first extended slot. */
    static const unsigned ARROW_THIS_SLOT = 0;

    static inline size_t offsetOfExtendedSlot(unsigned which) {
        MOZ_ASSERT(which < NUM_EXTENDED_SLOTS);
        return offsetof(FunctionExtended, extendedSlots) + which * sizeof(HeapValue);
    }
    static inline size_t offsetOfArrowThisSlot() {
        return offsetOfExtendedSlot(ARROW_THIS_SLOT);
    }

  private:
    friend class JSFunction;

    /* Reserved slots available for storage by particular native functions. */
    HeapValue extendedSlots[NUM_EXTENDED_SLOTS];
};

extern bool
CloneFunctionObjectUseSameScript(JSCompartment* compartment, HandleFunction fun);

extern JSFunction*
CloneFunctionObject(JSContext* cx, HandleFunction fun, HandleObject parent,
                    gc::AllocKind kind = JSFunction::FinalizeKind,
                    NewObjectKind newKindArg = GenericObject);

extern bool
FindBody(JSContext* cx, HandleFunction fun, HandleLinearString src, size_t* bodyStart,
         size_t* bodyEnd);

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
    toExtended()->extendedSlots[which].init(val);
}

inline void
JSFunction::setExtendedSlot(size_t which, const js::Value& val)
{
    MOZ_ASSERT(which < mozilla::ArrayLength(toExtended()->extendedSlots));
    toExtended()->extendedSlots[which] = val;
}

inline const js::Value&
JSFunction::getExtendedSlot(size_t which) const
{
    MOZ_ASSERT(which < mozilla::ArrayLength(toExtended()->extendedSlots));
    return toExtended()->extendedSlots[which];
}

namespace js {

JSString* FunctionToString(JSContext* cx, HandleFunction fun, bool bodyOnly, bool lambdaParen);

template<XDRMode mode>
bool
XDRInterpretedFunction(XDRState<mode>* xdr, HandleObject enclosingScope,
                       HandleScript enclosingScript, MutableHandleFunction objp);

extern JSObject*
CloneFunctionAndScript(JSContext* cx, HandleObject enclosingScope, HandleFunction fun);

/*
 * Report an error that call.thisv is not compatible with the specified class,
 * assuming that the method (clasp->name).prototype.<name of callee function>
 * is what was called.
 */
extern void
ReportIncompatibleMethod(JSContext* cx, CallReceiver call, const Class* clasp);

/*
 * Report an error that call.thisv is not an acceptable this for the callee
 * function.
 */
extern void
ReportIncompatible(JSContext* cx, CallReceiver call);

bool
CallOrConstructBoundFunction(JSContext*, unsigned, js::Value*);

extern const JSFunctionSpec function_methods[];

} /* namespace js */

extern bool
js_fun_apply(JSContext* cx, unsigned argc, js::Value* vp);

extern bool
js_fun_call(JSContext* cx, unsigned argc, js::Value* vp);

extern JSObject*
js_fun_bind(JSContext* cx, js::HandleObject target, js::HandleValue thisArg,
            js::Value* boundArgs, unsigned argslen);

#ifdef DEBUG
namespace JS {
namespace detail {

JS_PUBLIC_API(void)
CheckIsValidConstructible(Value calleev);

} // namespace detail
} // namespace JS
#endif

#endif /* jsfun_h */
