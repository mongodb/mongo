/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Extensions to the Result type to enable simpler handling of XPCOM/NSPR
 * results. */

#ifndef mozilla_ResultExtensions_h
#define mozilla_ResultExtensions_h

#include "mozilla/Assertions.h"
#include "nscore.h"
#include "prtypes.h"
#include "mozilla/dom/quota/RemoveParen.h"

namespace mozilla {

struct ErrorPropagationTag;

// Allow nsresult errors to automatically convert to nsresult values, so MOZ_TRY
// can be used in XPCOM methods with Result<T, nserror> results.
template <>
class [[nodiscard]] GenericErrorResult<nsresult> {
  nsresult mErrorValue;

  template <typename V, typename E2>
  friend class Result;

 public:
  explicit GenericErrorResult(nsresult aErrorValue) : mErrorValue(aErrorValue) {
    MOZ_ASSERT(NS_FAILED(aErrorValue));
  }

  GenericErrorResult(nsresult aErrorValue, const ErrorPropagationTag&)
      : GenericErrorResult(aErrorValue) {}

  operator nsresult() const { return mErrorValue; }
};

// Allow MOZ_TRY to handle `PRStatus` values.
template <typename E = nsresult>
inline Result<Ok, E> ToResult(PRStatus aValue);

}  // namespace mozilla

#include "mozilla/Result.h"

namespace mozilla {

template <typename ResultType>
struct ResultTypeTraits;

template <>
struct ResultTypeTraits<nsresult> {
  static nsresult From(nsresult aValue) { return aValue; }
};

template <typename E>
inline Result<Ok, E> ToResult(nsresult aValue) {
  if (NS_FAILED(aValue)) {
    return Err(ResultTypeTraits<E>::From(aValue));
  }
  return Ok();
}

template <typename E>
inline Result<Ok, E> ToResult(PRStatus aValue) {
  if (aValue == PR_SUCCESS) {
    return Ok();
  }
  return Err(ResultTypeTraits<E>::From(NS_ERROR_FAILURE));
}

namespace detail {
template <typename R>
auto ResultRefAsParam(R& aResult) {
  return &aResult;
}

template <typename R, typename E, typename RArgMapper, typename Func,
          typename... Args>
Result<R, E> ToResultInvokeInternal(const Func& aFunc,
                                    const RArgMapper& aRArgMapper,
                                    Args&&... aArgs) {
  // XXX Thereotically, if R is a pointer to a non-refcounted type, this might
  // be a non-owning pointer, but unless we find a case where this actually is
  // relevant, it's safe to forbid any raw pointer result.
  static_assert(
      !std::is_pointer_v<R>,
      "Raw pointer results are not supported, please specify a smart pointer "
      "result type explicitly, so that getter_AddRefs is used");

  R res;
  nsresult rv = aFunc(std::forward<Args>(aArgs)..., aRArgMapper(res));
  if (NS_FAILED(rv)) {
    return Err(ResultTypeTraits<E>::From(rv));
  }
  return res;
}

template <typename T>
struct outparam_as_pointer;

template <typename T>
struct outparam_as_pointer<T*> {
  using type = T*;
};

template <typename T>
struct outparam_as_reference;

template <typename T>
struct outparam_as_reference<T*> {
  using type = T&;
};

template <typename R, typename E, template <typename> typename RArg,
          typename Func, typename... Args>
using to_result_retval_t =
    decltype(std::declval<Func&>()(
                 std::declval<Args&&>()...,
                 std::declval<typename RArg<decltype(ResultRefAsParam(
                     std::declval<R&>()))>::type>()),
             Result<R, E>(Err(ResultTypeTraits<E>::From(NS_ERROR_FAILURE))));

// There are two ToResultInvokeSelector overloads, which cover the cases of a) a
// pointer-typed output parameter, and b) a reference-typed output parameter,
// using to_result_retval_t in connection with outparam_as_pointer and
// outparam_as_reference type traits. These type traits may be specialized for
// types other than raw pointers to allow calling functions with argument types
// that implicitly convert/bind to a raw pointer/reference. The overload that is
// used is selected by expression SFINAE: the decltype expression in
// to_result_retval_t is only valid in either case.
template <typename R, typename E, typename Func, typename... Args>
auto ToResultInvokeSelector(const Func& aFunc, Args&&... aArgs)
    -> to_result_retval_t<R, E, outparam_as_pointer, Func, Args...> {
  return ToResultInvokeInternal<R, E>(
      aFunc, [](R& res) -> decltype(auto) { return ResultRefAsParam(res); },
      std::forward<Args>(aArgs)...);
}

template <typename R, typename E, typename Func, typename... Args>
auto ToResultInvokeSelector(const Func& aFunc, Args&&... aArgs)
    -> to_result_retval_t<R, E, outparam_as_reference, Func, Args...> {
  return ToResultInvokeInternal<R, E>(
      aFunc, [](R& res) -> decltype(auto) { return *ResultRefAsParam(res); },
      std::forward<Args>(aArgs)...);
}

}  // namespace detail

/**
 * Adapts a function with a nsresult error type and an R* output parameter as
 * the last parameter to a function returning a mozilla::Result<R, nsresult>
 * object.
 *
 * This can also be used with member functions together with std::men_fn, e.g.
 *
 *    nsCOMPtr<nsIFile> file = ...;
 *    auto existsOrErr = ToResultInvoke<bool>(std::mem_fn(&nsIFile::Exists),
 *                                            *file);
 *
 * but it is more convenient to use the member function version, which has the
 * additional benefit of enabling the deduction of the success result type:
 *
 *    nsCOMPtr<nsIFile> file = ...;
 *    auto existsOrErr = ToResultInvokeMember(*file, &nsIFile::Exists);
 */
template <typename R, typename E = nsresult, typename Func, typename... Args>
Result<R, E> ToResultInvoke(const Func& aFunc, Args&&... aArgs) {
  return detail::ToResultInvokeSelector<R, E, Func, Args&&...>(
      aFunc, std::forward<Args>(aArgs)...);
}

namespace detail {
template <typename T>
struct tag {
  using type = T;
};

template <typename... Ts>
struct select_last {
  using type = typename decltype((tag<Ts>{}, ...))::type;
};

template <typename... Ts>
using select_last_t = typename select_last<Ts...>::type;

template <>
struct select_last<> {
  using type = void;
};

template <typename E, typename RArg, typename T, typename Func,
          typename... Args>
auto ToResultInvokeMemberInternal(T& aObj, const Func& aFunc, Args&&... aArgs) {
  if constexpr (std::is_pointer_v<RArg> ||
                (std::is_lvalue_reference_v<RArg> &&
                 !std::is_const_v<std::remove_reference_t<RArg>>)) {
    auto lambda = [&](RArg res) {
      return (aObj.*aFunc)(std::forward<Args>(aArgs)..., res);
    };
    return detail::ToResultInvokeSelector<
        std::remove_reference_t<std::remove_pointer_t<RArg>>, E,
        decltype(lambda)>(lambda);
  } else {
    // No output parameter present, return a Result<Ok, E>
    return mozilla::ToResult<E>((aObj.*aFunc)(std::forward<Args>(aArgs)...));
  }
}

// For use in MOZ_TO_RESULT_INVOKE_MEMBER/MOZ_TO_RESULT_INVOKE_MEMBER_TYPED.
template <typename T>
auto DerefHelper(const T&) -> T&;

template <typename T>
auto DerefHelper(T*) -> T&;

template <template <class> class SmartPtr, typename T,
          typename = decltype(*std::declval<const SmartPtr<T>>())>
auto DerefHelper(const SmartPtr<T>&) -> T&;

template <typename T>
using DerefedType =
    std::remove_reference_t<decltype(DerefHelper(std::declval<const T&>()))>;
}  // namespace detail

template <typename E = nsresult, typename T, typename U, typename... XArgs,
          typename... Args,
          typename = std::enable_if_t<std::is_base_of_v<U, T>>>
auto ToResultInvokeMember(T& aObj, nsresult (U::*aFunc)(XArgs...),
                          Args&&... aArgs) {
  return detail::ToResultInvokeMemberInternal<E,
                                              detail::select_last_t<XArgs...>>(
      aObj, aFunc, std::forward<Args>(aArgs)...);
}

template <typename E = nsresult, typename T, typename U, typename... XArgs,
          typename... Args,
          typename = std::enable_if_t<std::is_base_of_v<U, T>>>
auto ToResultInvokeMember(const T& aObj, nsresult (U::*aFunc)(XArgs...) const,
                          Args&&... aArgs) {
  return detail::ToResultInvokeMemberInternal<E,
                                              detail::select_last_t<XArgs...>>(
      aObj, aFunc, std::forward<Args>(aArgs)...);
}

template <typename E = nsresult, typename T, typename U, typename... XArgs,
          typename... Args>
auto ToResultInvokeMember(T* const aObj, nsresult (U::*aFunc)(XArgs...),
                          Args&&... aArgs) {
  return ToResultInvokeMember<E>(*aObj, aFunc, std::forward<Args>(aArgs)...);
}

template <typename E = nsresult, typename T, typename U, typename... XArgs,
          typename... Args>
auto ToResultInvokeMember(const T* const aObj,
                          nsresult (U::*aFunc)(XArgs...) const,
                          Args&&... aArgs) {
  return ToResultInvokeMember<E>(*aObj, aFunc, std::forward<Args>(aArgs)...);
}

template <typename E = nsresult, template <class> class SmartPtr, typename T,
          typename U, typename... XArgs, typename... Args,
          typename = std::enable_if_t<std::is_base_of_v<U, T>>,
          typename = decltype(*std::declval<const SmartPtr<T>>())>
auto ToResultInvokeMember(const SmartPtr<T>& aObj,
                          nsresult (U::*aFunc)(XArgs...), Args&&... aArgs) {
  return ToResultInvokeMember<E>(*aObj, aFunc, std::forward<Args>(aArgs)...);
}

template <typename E = nsresult, template <class> class SmartPtr, typename T,
          typename U, typename... XArgs, typename... Args,
          typename = std::enable_if_t<std::is_base_of_v<U, T>>,
          typename = decltype(*std::declval<const SmartPtr<T>>())>
auto ToResultInvokeMember(const SmartPtr<const T>& aObj,
                          nsresult (U::*aFunc)(XArgs...) const,
                          Args&&... aArgs) {
  return ToResultInvokeMember<E>(*aObj, aFunc, std::forward<Args>(aArgs)...);
}

#if defined(XP_WIN) && !defined(_WIN64)
template <typename E = nsresult, typename T, typename U, typename... XArgs,
          typename... Args,
          typename = std::enable_if_t<std::is_base_of_v<U, T>>>
auto ToResultInvokeMember(T& aObj, nsresult (__stdcall U::*aFunc)(XArgs...),
                          Args&&... aArgs) {
  return detail::ToResultInvokeMemberInternal<E,
                                              detail::select_last_t<XArgs...>>(
      aObj, aFunc, std::forward<Args>(aArgs)...);
}

template <typename E = nsresult, typename T, typename U, typename... XArgs,
          typename... Args,
          typename = std::enable_if_t<std::is_base_of_v<U, T>>>
auto ToResultInvokeMember(const T& aObj,
                          nsresult (__stdcall U::*aFunc)(XArgs...) const,
                          Args&&... aArgs) {
  return detail::ToResultInvokeMemberInternal<E,
                                              detail::select_last_t<XArgs...>>(
      aObj, aFunc, std::forward<Args>(aArgs)...);
}

template <typename E = nsresult, typename T, typename U, typename... XArgs,
          typename... Args>
auto ToResultInvokeMember(T* const aObj,
                          nsresult (__stdcall U::*aFunc)(XArgs...),
                          Args&&... aArgs) {
  return ToResultInvokeMember<E>(*aObj, aFunc, std::forward<Args>(aArgs)...);
}

template <typename E = nsresult, typename T, typename U, typename... XArgs,
          typename... Args>
auto ToResultInvokeMember(const T* const aObj,
                          nsresult (__stdcall U::*aFunc)(XArgs...) const,
                          Args&&... aArgs) {
  return ToResultInvokeMember<E>(*aObj, aFunc, std::forward<Args>(aArgs)...);
}

template <typename E = nsresult, template <class> class SmartPtr, typename T,
          typename U, typename... XArgs, typename... Args,
          typename = std::enable_if_t<std::is_base_of_v<U, T>>,
          typename = decltype(*std::declval<const SmartPtr<T>>())>
auto ToResultInvokeMember(const SmartPtr<T>& aObj,
                          nsresult (__stdcall U::*aFunc)(XArgs...),
                          Args&&... aArgs) {
  return ToResultInvokeMember<E>(*aObj, aFunc, std::forward<Args>(aArgs)...);
}

template <typename E = nsresult, template <class> class SmartPtr, typename T,
          typename U, typename... XArgs, typename... Args,
          typename = std::enable_if_t<std::is_base_of_v<U, T>>,
          typename = decltype(*std::declval<const SmartPtr<T>>())>
auto ToResultInvokeMember(const SmartPtr<const T>& aObj,
                          nsresult (__stdcall U::*aFunc)(XArgs...) const,
                          Args&&... aArgs) {
  return ToResultInvokeMember<E>(*aObj, aFunc, std::forward<Args>(aArgs)...);
}
#endif

// Macro version of ToResultInvokeMember for member functions. The macro has
// the advantage of not requiring spelling out the member function's declarator
// type name, at the expense of having a non-standard syntax. It can be used
// like this:
//
//     nsCOMPtr<nsIFile> file;
//     auto existsOrErr = MOZ_TO_RESULT_INVOKE_MEMBER(file, Exists);
#define MOZ_TO_RESULT_INVOKE_MEMBER(obj, methodname, ...)                \
  ::mozilla::ToResultInvokeMember(                                       \
      (obj), &::mozilla::detail::DerefedType<decltype(obj)>::methodname, \
      ##__VA_ARGS__)

// Macro version of ToResultInvokeMember for member functions, where the result
// type does not match the output parameter type. The macro has the advantage
// of not requiring spelling out the member function's declarator type name, at
// the expense of having a non-standard syntax. It can be used like this:
//
//     nsCOMPtr<nsIFile> file;
//     auto existsOrErr =
//         MOZ_TO_RESULT_INVOKE_MEMBER_TYPED(nsCOMPtr<nsIFile>, file, Clone);
#define MOZ_TO_RESULT_INVOKE_MEMBER_TYPED(resultType, obj, methodname, ...) \
  ::mozilla::ToResultInvoke<MOZ_REMOVE_PAREN(resultType)>(                  \
      ::std::mem_fn(                                                        \
          &::mozilla::detail::DerefedType<decltype(obj)>::methodname),      \
      (obj), ##__VA_ARGS__)

}  // namespace mozilla

#endif  // mozilla_ResultExtensions_h
