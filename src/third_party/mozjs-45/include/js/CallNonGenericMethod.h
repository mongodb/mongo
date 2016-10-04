/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_CallNonGenericMethod_h
#define js_CallNonGenericMethod_h

#include "jstypes.h"

#include "js/CallArgs.h"

namespace JS {

// Returns true if |v| is considered an acceptable this-value.
typedef bool (*IsAcceptableThis)(HandleValue v);

// Implements the guts of a method; guaranteed to be provided an acceptable
// this-value, as determined by a corresponding IsAcceptableThis method.
typedef bool (*NativeImpl)(JSContext* cx, const CallArgs& args);

namespace detail {

// DON'T CALL THIS DIRECTLY.  It's for use only by CallNonGenericMethod!
extern JS_PUBLIC_API(bool)
CallMethodIfWrapped(JSContext* cx, IsAcceptableThis test, NativeImpl impl, const CallArgs& args);

} // namespace detail

// Methods usually act upon |this| objects only from a single global object and
// compartment.  Sometimes, however, a method must act upon |this| values from
// multiple global objects or compartments.  In such cases the |this| value a
// method might see will be wrapped, such that various access to the object --
// to its class, its private data, its reserved slots, and so on -- will not
// work properly without entering that object's compartment.  This method
// implements a solution to this problem.
//
// To implement a method that accepts |this| values from multiple compartments,
// define two functions.  The first function matches the IsAcceptableThis type
// and indicates whether the provided value is an acceptable |this| for the
// method; it must be a pure function only of its argument.
//
//   static const JSClass AnswerClass = { ... };
//
//   static bool
//   IsAnswerObject(const Value& v)
//   {
//       if (!v.isObject())
//           return false;
//       return JS_GetClass(&v.toObject()) == &AnswerClass;
//   }
//
// The second function implements the NativeImpl signature and defines the
// behavior of the method when it is provided an acceptable |this| value.
// Aside from some typing niceties -- see the CallArgs interface for details --
// its interface is the same as that of JSNative.
//
//   static bool
//   answer_getAnswer_impl(JSContext* cx, JS::CallArgs args)
//   {
//       args.rval().setInt32(42);
//       return true;
//   }
//
// The implementation function is guaranteed to be called *only* with a |this|
// value which is considered acceptable.
//
// Now to implement the actual method, write a JSNative that calls the method
// declared below, passing the appropriate template and runtime arguments.
//
//   static bool
//   answer_getAnswer(JSContext* cx, unsigned argc, JS::Value* vp)
//   {
//       JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
//       return JS::CallNonGenericMethod<IsAnswerObject, answer_getAnswer_impl>(cx, args);
//   }
//
// Note that, because they are used as template arguments, the predicate
// and implementation functions must have external linkage. (This is
// unfortunate, but GCC wasn't inlining things as one would hope when we
// passed them as function arguments.)
//
// JS::CallNonGenericMethod will test whether |args.thisv()| is acceptable.  If
// it is, it will call the provided implementation function, which will return
// a value and indicate success.  If it is not, it will attempt to unwrap
// |this| and call the implementation function on the unwrapped |this|.  If
// that succeeds, all well and good.  If it doesn't succeed, a TypeError will
// be thrown.
//
// Note: JS::CallNonGenericMethod will only work correctly if it's called in
//       tail position in a JSNative.  Do not call it from any other place.
//
template<IsAcceptableThis Test, NativeImpl Impl>
MOZ_ALWAYS_INLINE bool
CallNonGenericMethod(JSContext* cx, const CallArgs& args)
{
    HandleValue thisv = args.thisv();
    if (Test(thisv))
        return Impl(cx, args);

    return detail::CallMethodIfWrapped(cx, Test, Impl, args);
}

MOZ_ALWAYS_INLINE bool
CallNonGenericMethod(JSContext* cx, IsAcceptableThis Test, NativeImpl Impl, const CallArgs& args)
{
    HandleValue thisv = args.thisv();
    if (Test(thisv))
        return Impl(cx, args);

    return detail::CallMethodIfWrapped(cx, Test, Impl, args);
}

} // namespace JS

#endif /* js_CallNonGenericMethod_h */
