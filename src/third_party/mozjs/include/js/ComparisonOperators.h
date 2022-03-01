/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Support comparison operations on wrapper types -- e.g. |JS::Rooted<T>|,
 * |JS::Handle<T>|, and so on -- against raw |T| values, against pointers or
 * |nullptr| if the wrapper is a pointer wrapper, and against other wrappers
 * around compatible types.
 */

#ifndef js_ComparisonOperators_h
#define js_ComparisonOperators_h

#include <type_traits>  // std::false_type, std::true_type, std::enable_if_t, std::is_pointer_v, std::remove_pointer_t

// To define |operator==| and |operator!=| for a wrapper class |W| (which may
// or may not be a template class) that contains a |T|:
//
//   * Specialize |JS::detail::DefineComparisonOps| for |W|:
//     - Make it inherit from |std::true_type|.
//     - Include within your specialization a |static get(const W& v)| function
//       that returns the value (which may be an lvalue reference) of the |T| in
//       |W|.
//   * If needed, add |using JS::detail::wrapper_comparison::operator==;| and
//     |using JS::detail::wrapper_comparison::operator!=;| to the namespace
//     directly containing |W| at the end of this header.  (If you are not in
//     SpiderMonkey code and have questionably decided to define your own
//     wrapper class, add these to its namespace somewhere in your code.)
//
// The first step opts the wrapper class into comparison support and defines a
// generic means of extracting a comparable |T| out of an instance.
//
// The second step ensures that symmetric |operator==| and |operator!=| are
// exposed for the wrapper, accepting two wrappers or a wrapper and a suitable
// raw value.
//
// Failure to perform *both* steps will likely result in errors like
// 'invalid operands to binary expression' or 'no match for operator=='
// when comparing an instance of your wrapper.

namespace JS {

namespace detail {

// By default, comparison ops are not supported for types.
template <typename T>
struct DefineComparisonOps : std::false_type {};

// Define functions for the core equality operations, that the actual operators
// can all invoke.

// Compare two wrapper types.  Assumes both wrapper types support comparison
// operators.
template <typename W, typename OW>
inline bool WrapperEqualsWrapper(const W& wrapper, const OW& other) {
  return JS::detail::DefineComparisonOps<W>::get(wrapper) ==
         JS::detail::DefineComparisonOps<OW>::get(other);
}

// Compare a wrapper against a value of its unwrapped element type (or against a
// value that implicitly converts to that unwrapped element type).  Assumes its
// wrapper argument supports comparison operators.
template <typename W>
inline bool WrapperEqualsUnwrapped(const W& wrapper,
                                   const typename W::ElementType& value) {
  return JS::detail::DefineComparisonOps<W>::get(wrapper) == value;
}

// Compare a wrapper containing a pointer against a pointer to const element
// type.  Assumes its wrapper argument supports comparison operators.
template <typename W>
inline bool WrapperEqualsPointer(
    const W& wrapper,
    const typename std::remove_pointer_t<typename W::ElementType>* ptr) {
  return JS::detail::DefineComparisonOps<W>::get(wrapper) == ptr;
}

// It is idiomatic C++ to define operators on user-defined types in the
// namespace of their operands' types (not at global scope, which isn't examined
// if at point of operator use another operator definition shadows the global
// definition).  But our wrappers live in *multiple* namespaces (|namespace js|
// and |namespace JS| in SpiderMonkey), so we can't literally do that without
// defining ambiguous overloads.
//
// Instead, we define the operators *once* in a namespace containing nothing
// else at all.  Then we |using| the operators into each namespace containing
// a wrapper type.  |using| creates *aliases*, so two |using|s of the same
// operator contribute only one overload to overload resolution.
namespace wrapper_comparison {

// Comparisons between potentially-differing wrappers.
template <typename W, typename OW>
inline typename std::enable_if_t<JS::detail::DefineComparisonOps<W>::value &&
                                     JS::detail::DefineComparisonOps<OW>::value,
                                 bool>
operator==(const W& wrapper, const OW& other) {
  return JS::detail::WrapperEqualsWrapper(wrapper, other);
}

template <typename W, typename OW>
inline typename std::enable_if_t<JS::detail::DefineComparisonOps<W>::value &&
                                     JS::detail::DefineComparisonOps<OW>::value,
                                 bool>
operator!=(const W& wrapper, const OW& other) {
  return !JS::detail::WrapperEqualsWrapper(wrapper, other);
}

// Comparisons between a wrapper and its unwrapped element type.
template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value, bool>
operator==(const W& wrapper, const typename W::ElementType& value) {
  return WrapperEqualsUnwrapped(wrapper, value);
}

template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value, bool>
operator!=(const W& wrapper, const typename W::ElementType& value) {
  return !WrapperEqualsUnwrapped(wrapper, value);
}

template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value, bool>
operator==(const typename W::ElementType& value, const W& wrapper) {
  return WrapperEqualsUnwrapped(wrapper, value);
}

template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value, bool>
operator!=(const typename W::ElementType& value, const W& wrapper) {
  return !WrapperEqualsUnwrapped(wrapper, value);
}

// For wrappers around a pointer type, comparisons between a wrapper object
// and a const element pointer.
template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value &&
                                     std::is_pointer_v<typename W::ElementType>,
                                 bool>
operator==(const W& wrapper,
           const typename std::remove_pointer_t<typename W::ElementType>* ptr) {
  return WrapperEqualsPointer(wrapper, ptr);
}

template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value &&
                                     std::is_pointer_v<typename W::ElementType>,
                                 bool>
operator!=(const W& wrapper,
           const typename std::remove_pointer_t<typename W::ElementType>* ptr) {
  return !WrapperEqualsPointer(wrapper, ptr);
}

template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value &&
                                     std::is_pointer_v<typename W::ElementType>,
                                 bool>
operator==(const typename std::remove_pointer_t<typename W::ElementType>* ptr,
           const W& wrapper) {
  return WrapperEqualsPointer(wrapper, ptr);
}

template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value &&
                                     std::is_pointer_v<typename W::ElementType>,
                                 bool>
operator!=(const typename std::remove_pointer_t<typename W::ElementType>* ptr,
           const W& wrapper) {
  return !WrapperEqualsPointer(wrapper, ptr);
}

// For wrappers around a pointer type, comparisons between a wrapper object
// and |nullptr|.
//
// These overloads are a workaround for gcc hazard build bugs.  Per spec,
// |nullptr -> const T*| for the wrapper-pointer operators immediately above
// this is a standard conversion sequence (consisting of a single pointer
// conversion).  Meanwhile, |nullptr -> T* const&| for the wrapper-element
// operators just above that, is a pointer conversion to |T*|, then an identity
// conversion of the |T* const| to a reference.  The former conversion sequence
// is a proper subsequence of the latter, so it *should* be a better conversion
// sequence and thus should be the better overload.  But gcc doesn't implement
// things this way, so we add overloads directly targeting |nullptr| as an exact
// match, preferred to either of those overloads.
//
// We should be able to remove these overloads when gcc hazard builds use modern
// clang.
template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value, bool>
operator==(const W& wrapper, std::nullptr_t) {
  return WrapperEqualsUnwrapped(wrapper, nullptr);
}

template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value, bool>
operator!=(const W& wrapper, std::nullptr_t) {
  return !WrapperEqualsUnwrapped(wrapper, nullptr);
}

template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value, bool>
operator==(std::nullptr_t, const W& wrapper) {
  return WrapperEqualsUnwrapped(wrapper, nullptr);
}

template <typename W>
inline typename std::enable_if_t<DefineComparisonOps<W>::value, bool>
operator!=(std::nullptr_t, const W& wrapper) {
  return !WrapperEqualsUnwrapped(wrapper, nullptr);
}

}  // namespace wrapper_comparison

}  // namespace detail

}  // namespace JS

// Expose wrapper-supporting |operator==| and |operator!=| in the namespaces of
// all SpiderMonkey's wrapper classes that support comparisons.

namespace JS {

using JS::detail::wrapper_comparison::operator==;
using JS::detail::wrapper_comparison::operator!=;

}  // namespace JS

namespace js {

using JS::detail::wrapper_comparison::operator==;
using JS::detail::wrapper_comparison::operator!=;

}  // namespace js

#endif  // js_ComparisonOperators_h
