/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Helper classes encapsulating access to the callee, |this| value, arguments,
 * and argument count for a call/construct operation.
 *
 * JS::CallArgs encapsulates access to a JSNative's un-abstracted
 * |unsigned argc, Value* vp| arguments.  The principal way to create a
 * JS::CallArgs is using JS::CallArgsFromVp:
 *
 *   // If provided no arguments or a non-numeric first argument, return zero.
 *   // Otherwise return |this| exactly as given, without boxing.
 *   static bool
 *   Func(JSContext* cx, unsigned argc, JS::Value* vp)
 *   {
 *       JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
 *
 *       // Guard against no arguments or a non-numeric arg0.
 *       if (args.length() == 0 || !args[0].isNumber()) {
 *           args.rval().setInt32(0);
 *           return true;
 *       }
 *
 *       // Access to the callee must occur before accessing/setting
 *       // the return value.
 *       JSObject& callee = args.callee();
 *       args.rval().setObject(callee);
 *
 *       // callee() and calleev() will now assert.
 *
 *       // It's always fine to access thisv().
 *       HandleValue thisv = args.thisv();
 *       args.rval().set(thisv);
 *
 *       // As the return value was last set to |this|, returns |this|.
 *       return true;
 *   }
 *
 * CallArgs is exposed publicly and used internally.  Not all parts of its
 * public interface are meant to be used by embedders!  See inline comments to
 * for details.
 *
 * It's possible (albeit deprecated) to manually index into |vp| to access the
 * callee, |this|, and arguments of a function, and to set its return value.
 * It's also possible to use the supported API of JS_CALLEE, JS_THIS, JS_ARGV,
 * JS_RVAL, and JS_SET_RVAL to the same ends.
 *
 * But neither API has the error-handling or moving-GC correctness of CallArgs.
 * New code should use CallArgs instead whenever possible.
 *
 * The eventual plan is to change JSNative to take |const CallArgs&| directly,
 * for automatic assertion of correct use and to make calling functions more
 * efficient.  Embedders should start internally switching away from using
 * |argc| and |vp| directly, except to create a |CallArgs|.  Then, when an
 * eventual release making that change occurs, porting efforts will require
 * changing methods' signatures but won't require invasive changes to the
 * methods' implementations, potentially under time pressure.
 */

#ifndef js_CallArgs_h
#define js_CallArgs_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/TypeTraits.h"

#include "jstypes.h"

#include "js/RootingAPI.h"
#include "js/Value.h"

/* Typedef for native functions called by the JS VM. */
typedef bool
(* JSNative)(JSContext* cx, unsigned argc, JS::Value* vp);

namespace JS {

extern JS_PUBLIC_DATA(const HandleValue) UndefinedHandleValue;

namespace detail {

/*
 * Compute |this| for the |vp| inside a JSNative, either boxing primitives or
 * replacing with the global object as necessary.
 */
extern JS_PUBLIC_API(Value)
ComputeThis(JSContext* cx, JS::Value* vp);

#ifdef JS_DEBUG
extern JS_PUBLIC_API(void)
CheckIsValidConstructible(const Value& v);
#endif

class MOZ_STACK_CLASS IncludeUsedRval
{
    mutable bool usedRval_;

  public:
    bool usedRval() const { return usedRval_; }
    void setUsedRval() const { usedRval_ = true; }
    void clearUsedRval() const { usedRval_ = false; }
    void assertUnusedRval() const { MOZ_ASSERT(!usedRval_); }
};

class MOZ_STACK_CLASS NoUsedRval
{
  public:
    bool usedRval() const { return false; }
    void setUsedRval() const {}
    void clearUsedRval() const {}
    void assertUnusedRval() const {}
};

template<class WantUsedRval>
class MOZ_STACK_CLASS CallArgsBase
{
    static_assert(mozilla::IsSame<WantUsedRval, IncludeUsedRval>::value ||
                  mozilla::IsSame<WantUsedRval, NoUsedRval>::value,
                  "WantUsedRval can only be IncludeUsedRval or NoUsedRval");

  protected:
    Value* argv_;
    unsigned argc_;
    bool constructing_:1;

    // True if the caller does not use the return value.
    bool ignoresReturnValue_:1;

#ifdef JS_DEBUG
    WantUsedRval wantUsedRval_;
    bool usedRval() const { return wantUsedRval_.usedRval(); }
    void setUsedRval() const { wantUsedRval_.setUsedRval(); }
    void clearUsedRval() const { wantUsedRval_.clearUsedRval(); }
    void assertUnusedRval() const { wantUsedRval_.assertUnusedRval(); }
#else
    bool usedRval() const { return false; }
    void setUsedRval() const {}
    void clearUsedRval() const {}
    void assertUnusedRval() const {}
#endif

  public:
    // CALLEE ACCESS

    /*
     * Returns the function being called, as a value.  Must not be called after
     * rval() has been used!
     */
    HandleValue calleev() const {
        this->assertUnusedRval();
        return HandleValue::fromMarkedLocation(&argv_[-2]);
    }

    /*
     * Returns the function being called, as an object.  Must not be called
     * after rval() has been used!
     */
    JSObject& callee() const {
        return calleev().toObject();
    }

    // CALLING/CONSTRUCTING-DIFFERENTIATIONS

    bool isConstructing() const {
        if (!argv_[-1].isMagic())
            return false;

#ifdef JS_DEBUG
        if (!this->usedRval())
            CheckIsValidConstructible(calleev());
#endif

        return true;
    }

    bool ignoresReturnValue() const {
        return ignoresReturnValue_;
    }

    MutableHandleValue newTarget() const {
        MOZ_ASSERT(constructing_);
        return MutableHandleValue::fromMarkedLocation(&this->argv_[argc_]);
    }

    /*
     * Returns the |this| value passed to the function.  This method must not
     * be called when the function is being called as a constructor via |new|.
     * The value may or may not be an object: it is the individual function's
     * responsibility to box the value if needed.
     */
    HandleValue thisv() const {
        // Some internal code uses thisv() in constructing cases, so don't do
        // this yet.
        // MOZ_ASSERT(!argv_[-1].isMagic(JS_IS_CONSTRUCTING));
        return HandleValue::fromMarkedLocation(&argv_[-1]);
    }

    Value computeThis(JSContext* cx) const {
        if (thisv().isObject())
            return thisv();

        return ComputeThis(cx, base());
    }

    // ARGUMENTS

        /* Returns the number of arguments. */
    unsigned length() const { return argc_; }

    /* Returns the i-th zero-indexed argument. */
    MutableHandleValue operator[](unsigned i) const {
        MOZ_ASSERT(i < argc_);
        return MutableHandleValue::fromMarkedLocation(&this->argv_[i]);
    }

    /*
     * Returns the i-th zero-indexed argument, or |undefined| if there's no
     * such argument.
     */
    HandleValue get(unsigned i) const {
        return i < length()
               ? HandleValue::fromMarkedLocation(&this->argv_[i])
               : UndefinedHandleValue;
    }

    /*
     * Returns true if the i-th zero-indexed argument is present and is not
     * |undefined|.
     */
    bool hasDefined(unsigned i) const {
        return i < argc_ && !this->argv_[i].isUndefined();
    }

    // RETURN VALUE

    /*
     * Returns the currently-set return value.  The initial contents of this
     * value are unspecified.  Once this method has been called, callee() and
     * calleev() can no longer be used.  (If you're compiling against a debug
     * build of SpiderMonkey, these methods will assert to aid debugging.)
     *
     * If the method you're implementing succeeds by returning true, you *must*
     * set this.  (SpiderMonkey doesn't currently assert this, but it will do
     * so eventually.)  You don't need to use or change this if your method
     * fails.
     */
    MutableHandleValue rval() const {
        this->setUsedRval();
        return MutableHandleValue::fromMarkedLocation(&argv_[-2]);
    }

  public:
    // These methods are publicly exposed, but they are *not* to be used when
    // implementing a JSNative method and encapsulating access to |vp| within
    // it.  You probably don't want to use these!

    void setCallee(const Value& aCalleev) const {
        this->clearUsedRval();
        argv_[-2] = aCalleev;
    }

    void setThis(const Value& aThisv) const {
        argv_[-1] = aThisv;
    }

    MutableHandleValue mutableThisv() const {
        return MutableHandleValue::fromMarkedLocation(&argv_[-1]);
    }

  public:
    // These methods are publicly exposed, but we're unsure of the interfaces
    // (because they're hackish and drop assertions).  Avoid using these if you
    // can.

    Value* array() const { return argv_; }
    Value* end() const { return argv_ + argc_ + constructing_; }

  public:
    // These methods are only intended for internal use.  Embedders shouldn't
    // use them!

    Value* base() const { return argv_ - 2; }

    Value* spAfterCall() const {
        this->setUsedRval();
        return argv_ - 1;
    }
};

} // namespace detail

class MOZ_STACK_CLASS CallArgs : public detail::CallArgsBase<detail::IncludeUsedRval>
{
  private:
    friend CallArgs CallArgsFromVp(unsigned argc, Value* vp);
    friend CallArgs CallArgsFromSp(unsigned stackSlots, Value* sp, bool constructing,
                                   bool ignoresReturnValue);

    static CallArgs create(unsigned argc, Value* argv, bool constructing,
                           bool ignoresReturnValue = false) {
        CallArgs args;
        args.clearUsedRval();
        args.argv_ = argv;
        args.argc_ = argc;
        args.constructing_ = constructing;
        args.ignoresReturnValue_ = ignoresReturnValue;
#ifdef DEBUG
        MOZ_ASSERT(ValueIsNotGray(args.thisv()));
        MOZ_ASSERT(ValueIsNotGray(args.calleev()));
        for (unsigned i = 0; i < argc; ++i)
            MOZ_ASSERT(ValueIsNotGray(argv[i]));
#endif
        return args;
    }

  public:
    /*
     * Returns true if there are at least |required| arguments passed in. If
     * false, it reports an error message on the context.
     */
    JS_PUBLIC_API(bool) requireAtLeast(JSContext* cx, const char* fnname, unsigned required) const;

};

MOZ_ALWAYS_INLINE CallArgs
CallArgsFromVp(unsigned argc, Value* vp)
{
    return CallArgs::create(argc, vp + 2, vp[1].isMagic(JS_IS_CONSTRUCTING));
}

// This method is only intended for internal use in SpiderMonkey.  We may
// eventually move it to an internal header.  Embedders should use
// JS::CallArgsFromVp!
MOZ_ALWAYS_INLINE CallArgs
CallArgsFromSp(unsigned stackSlots, Value* sp, bool constructing = false,
               bool ignoresReturnValue = false)
{
    return CallArgs::create(stackSlots - constructing, sp - stackSlots, constructing,
                            ignoresReturnValue);
}

} // namespace JS

/*
 * Macros to hide interpreter stack layout details from a JSNative using its
 * JS::Value* vp parameter.  DO NOT USE THESE!  Instead use JS::CallArgs and
 * friends, above.  These macros will be removed when we change JSNative to
 * take a const JS::CallArgs&.
 */

/*
 * Return |this| if |this| is an object.  Otherwise, return the global object
 * if |this| is null or undefined, and finally return a boxed version of any
 * other primitive.
 *
 * Note: if this method returns null, an error has occurred and must be
 * propagated or caught.
 */
MOZ_ALWAYS_INLINE JS::Value
JS_THIS(JSContext* cx, JS::Value* vp)
{
    return vp[1].isPrimitive() ? JS::detail::ComputeThis(cx, vp) : vp[1];
}

/*
 * A note on JS_THIS_OBJECT: no equivalent method is part of the CallArgs
 * interface, and we're unlikely to add one (functions shouldn't be implicitly
 * exposing the global object to arbitrary callers).  Continue using |vp|
 * directly for this case, but be aware this API will eventually be replaced
 * with a function that operates directly upon |args.thisv()|.
 */
#define JS_THIS_OBJECT(cx,vp)   (JS_THIS(cx,vp).toObjectOrNull())

/*
 * |this| is passed to functions in ES5 without change.  Functions themselves
 * do any post-processing they desire to box |this|, compute the global object,
 * &c.  This macro retrieves a function's unboxed |this| value.
 *
 * This macro must not be used in conjunction with JS_THIS or JS_THIS_OBJECT,
 * or vice versa.  Either use the provided this value with this macro, or
 * compute the boxed |this| value using those.  JS_THIS_VALUE must not be used
 * if the function is being called as a constructor.
 *
 * But: DO NOT USE THIS!  Instead use JS::CallArgs::thisv(), above.
 *
 */
#define JS_THIS_VALUE(cx,vp)    ((vp)[1])

#endif /* js_CallArgs_h */
