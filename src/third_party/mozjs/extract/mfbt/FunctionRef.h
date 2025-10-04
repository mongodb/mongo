/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * A generic callable type that can be initialized from any compatible callable,
 * suitable for use as a function argument for the duration of the function
 * call (and no longer).
 */

#ifndef mozilla_FunctionRef_h
#define mozilla_FunctionRef_h

#include "mozilla/OperatorNewExtensions.h"  // mozilla::NotNull, ::operator new

#include <cstddef>      // std::nullptr_t
#include <type_traits>  // std::{declval,integral_constant}, std::is_{convertible,same,void}_v, std::{enable_if,remove_reference,remove_cv}_t
#include <utility>      // std::forward

// This concept and its implementation are substantially inspired by foonathan's
// prior art:
//
// https://foonathan.net/2017/01/function-ref-implementation/
// https://github.com/foonathan/type_safe/blob/2017851053f8dd268372f1612865792c5c621570/include/type_safe/reference.hpp

namespace mozilla {

namespace detail {

// Template helper to determine if |Returned| is a return type compatible with
// |Required|: if the former converts to the latter, or if |Required| is |void|
// and nothing is returned.
template <typename Returned, typename Required>
using CompatibleReturnType =
    std::integral_constant<bool, std::is_void_v<Required> ||
                                     std::is_convertible_v<Returned, Required>>;

// Template helper to check if |Func| called with |Params| arguments returns
// a type compatible with |Ret|.
template <typename Func, typename Ret, typename... Params>
using EnableMatchingFunction = std::enable_if_t<
    CompatibleReturnType<
        decltype(std::declval<Func&>()(std::declval<Params>()...)), Ret>::value,
    int>;

struct MatchingFunctionPointerTag {};
struct MatchingFunctorTag {};
struct InvalidFunctorTag {};

// Template helper to determine the proper way to store |Callable|: as function
// pointer, as pointer to object, or unstorable.
template <typename Callable, typename Ret, typename... Params>
struct GetCallableTag {
  // Match the case where |Callable| is a compatible function pointer or
  // converts to one.  (|+obj| invokes such a conversion.)
  template <typename T>
  static MatchingFunctionPointerTag test(
      int, T& obj, EnableMatchingFunction<decltype(+obj), Ret, Params...> = 0);

  // Match the case where |Callable| is callable but can't be converted to a
  // function pointer.  (|short| is a worse match for 0 than |int|, causing the
  // function pointer match to be preferred if both apply.)
  template <typename T>
  static MatchingFunctorTag test(short, T& obj,
                                 EnableMatchingFunction<T, Ret, Params...> = 0);

  // Match all remaining cases.  (Any other match is preferred to an ellipsis
  // match.)
  static InvalidFunctorTag test(...);

  using Type = decltype(test(0, std::declval<Callable&>()));
};

// If the callable is |nullptr|, |std::declval<std::nullptr_t&>()| will be an
// error.  Provide a specialization for |nullptr| that will fail substitution.
template <typename Ret, typename... Params>
struct GetCallableTag<std::nullptr_t, Ret, Params...> {};

template <typename Result, typename Callable, typename Ret, typename... Params>
using EnableFunctionTag = std::enable_if_t<
    std::is_same_v<typename GetCallableTag<Callable, Ret, Params...>::Type,
                   Result>,
    int>;

}  // namespace detail

/**
 * An efficient, type-erasing, non-owning reference to a callable. It is
 * intended for use as the type of a function parameter that is not used after
 * the function in question returns.
 *
 * This class does not own the callable, so in general it is unsafe to store a
 * FunctionRef.
 */
template <typename Fn>
class MOZ_TEMPORARY_CLASS FunctionRef;

template <typename Ret, typename... Params>
class MOZ_TEMPORARY_CLASS FunctionRef<Ret(Params...)> {
  union Payload;

  // |FunctionRef| stores an adaptor function pointer, determined by the
  // arguments passed to the constructor.  That adaptor will perform the steps
  // needed to invoke the callable passed at construction time.
  using Adaptor = Ret (*)(const Payload& aPayload, Params... aParams);

  // If |FunctionRef|'s callable can be stored as a function pointer, that
  // function pointer is stored after being cast to this *different* function
  // pointer type.  |mAdaptor| then casts back to the original type to call it.
  // ([expr.reinterpret.cast]p6 guarantees that A->B->A function pointer casts
  // produce the original function pointer value.)  An outlandish signature is
  // used to emphasize that the exact function pointer type doesn't matter.
  using FuncPtr = Payload***** (*)(Payload*****);

  /**
   * An adaptor function (used by this class's function call operator) that
   * invokes the callable in |mPayload|, forwarding arguments and converting
   * return type as needed.
   */
  const Adaptor mAdaptor;

  /** Storage for the wrapped callable value. */
  union Payload {
    // This arm is used if |FunctionRef| is passed a compatible function pointer
    // or a lambda/callable that converts to a compatible function pointer.
    FuncPtr mFuncPtr;

    // This arm is used if |FunctionRef| is passed some other callable or
    // |nullptr|.
    void* mObject;
  } mPayload;

  template <typename RealFuncPtr>
  static Ret CallFunctionPointer(const Payload& aPayload,
                                 Params... aParams) noexcept {
    auto func = reinterpret_cast<RealFuncPtr>(aPayload.mFuncPtr);
    return static_cast<Ret>(func(std::forward<Params>(aParams)...));
  }

  template <typename Ret2, typename... Params2>
  FunctionRef(detail::MatchingFunctionPointerTag, Ret2 (*aFuncPtr)(Params2...))
      : mAdaptor(&CallFunctionPointer<Ret2 (*)(Params2...)>) {
    ::new (KnownNotNull, &mPayload.mFuncPtr)
        FuncPtr(reinterpret_cast<FuncPtr>(aFuncPtr));
  }

 public:
  /**
   * Construct a |FunctionRef| that's like a null function pointer that can't be
   * called.
   */
  MOZ_IMPLICIT FunctionRef(std::nullptr_t) noexcept : mAdaptor(nullptr) {
    // This is technically unnecessary, but it seems best to always initialize
    // a union arm.
    ::new (KnownNotNull, &mPayload.mObject) void*(nullptr);
  }

  FunctionRef() : FunctionRef(nullptr) {}

  /**
   * Constructs a |FunctionRef| from an object callable with |Params| arguments,
   * that returns a type convertible to |Ret|, where the callable isn't
   * convertible to function pointer (often because it contains some internal
   * state).  For example:
   *
   *   int x = 5;
   *   DoSomething([&x] { x++; });
   */
  template <typename Callable,
            typename = detail::EnableFunctionTag<detail::MatchingFunctorTag,
                                                 Callable, Ret, Params...>,
            typename std::enable_if_t<!std::is_same_v<
                std::remove_cv_t<std::remove_reference_t<Callable>>,
                FunctionRef>>* = nullptr>
  MOZ_IMPLICIT FunctionRef(Callable&& aCallable MOZ_LIFETIME_BOUND) noexcept
      : mAdaptor([](const Payload& aPayload, Params... aParams) {
          auto& func = *static_cast<std::remove_reference_t<Callable>*>(
              aPayload.mObject);
          return static_cast<Ret>(func(std::forward<Params>(aParams)...));
        }) {
    ::new (KnownNotNull, &mPayload.mObject) void*(&aCallable);
  }

  /**
   * Constructs a |FunctionRef| from an value callable with |Params| arguments,
   * that returns a type convertible to |Ret|, where the callable is stateless
   * and is (or is convertible to) a function pointer.  For example:
   *
   *   // Exact match
   *   double twice(double d) { return d * 2; }
   *   FunctionRef<double(double)> func1(&twice);
   *
   *   // Compatible match
   *   float thrice(long double d) { return static_cast<float>(d) * 3; }
   *   FunctionRef<double(double)> func2(&thrice);
   *
   *   // Non-generic lambdas that don't capture anything have a conversion
   *   // function to the appropriate function pointer type.
   *   FunctionRef<int(double)> f([](long double){ return 'c'; });
   */
  template <typename Callable,
            typename = detail::EnableFunctionTag<
                detail::MatchingFunctionPointerTag, Callable, Ret, Params...>>
  MOZ_IMPLICIT FunctionRef(const Callable& aCallable) noexcept
      : FunctionRef(detail::MatchingFunctionPointerTag{}, +aCallable) {}

  /** Call the callable stored in this with the given arguments. */
  Ret operator()(Params... params) const {
    return mAdaptor(mPayload, std::forward<Params>(params)...);
  }

  /** Return true iff this wasn't created from |nullptr|. */
  explicit operator bool() const noexcept { return mAdaptor != nullptr; }
};

} /* namespace mozilla */

#endif /* mozilla_FunctionRef_h */
