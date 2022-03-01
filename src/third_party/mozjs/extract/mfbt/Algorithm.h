/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A polyfill for `<algorithm>`. */

#ifndef mozilla_Algorithm_h
#define mozilla_Algorithm_h

#include "mozilla/Result.h"

#include <iterator>
#include <type_traits>

namespace mozilla {

// Returns true if all elements in the range [aFirst, aLast)
// satisfy the predicate aPred.
template <class Iter, class Pred>
constexpr bool AllOf(Iter aFirst, Iter aLast, Pred aPred) {
  for (; aFirst != aLast; ++aFirst) {
    if (!aPred(*aFirst)) {
      return false;
    }
  }
  return true;
}

// Like C++20's `std::any_of`.
template <typename Iter, typename Pred>
constexpr bool AnyOf(Iter aFirst, Iter aLast, Pred aPred) {
  for (; aFirst != aLast; ++aFirst) {
    if (aPred(*aFirst)) {
      return true;
    }
  }

  return false;
}

namespace detail {
template <typename Transform, typename SrcIter>
using ArrayElementTransformType = typename std::invoke_result_t<
    Transform, typename std::iterator_traits<SrcIter>::reference>;

template <typename Transform, typename SrcIter>
struct TransformTraits {
  using result_type = ArrayElementTransformType<Transform, SrcIter>;

  using result_ok_type = typename result_type::ok_type;
  using result_err_type = typename result_type::err_type;
};
}  // namespace detail

// An algorithm similar to TransformAbortOnErr combined with a condition that
// allows to skip elements. At most std::distance(aIter, aEnd) elements will be
// inserted into aDst.
//
// Type requirements, in addition to those specified in TransformAbortOnErr:
// - Cond must be compatible with signature
//   bool (const SrcIter::value_type&)
template <typename SrcIter, typename DstIter, typename Cond, typename Transform>
Result<Ok,
       typename detail::TransformTraits<Transform, SrcIter>::result_err_type>
TransformIfAbortOnErr(SrcIter aIter, SrcIter aEnd, DstIter aDst, Cond aCond,
                      Transform aTransform) {
  for (; aIter != aEnd; ++aIter) {
    if (!aCond(static_cast<std::add_const_t<
                   typename std::iterator_traits<SrcIter>::value_type>&>(
            *aIter))) {
      continue;
    }

    auto res = aTransform(*aIter);
    if (res.isErr()) {
      return Err(res.unwrapErr());
    }

    *aDst++ = res.unwrap();
  }
  return Ok{};
}

template <typename SrcRange, typename DstIter, typename Cond,
          typename Transform>
auto TransformIfAbortOnErr(SrcRange& aRange, DstIter aDst, Cond aCond,
                           Transform aTransform) {
  using std::begin;
  using std::end;
  return TransformIfAbortOnErr(begin(aRange), end(aRange), aDst, aCond,
                               aTransform);
}

// An algorithm similar to std::transform, adapted to error handling based on
// mozilla::Result<V, E>. It iterates through the input range [aIter, aEnd) and
// inserts the result of applying aTransform to each element into aDst, if
// aTransform returns a success result. On the first error result, iterating is
// aborted, and the error result is returned as an overall result. If all
// transformations return a success result, Ok is returned as an overall result.
//
// Type requirements:
// - SrcIter must be an InputIterator.
// - DstIter must be an OutputIterator.
// - Transform must be compatible with signature
//   Result<DstIter::value_type, E> (SrcIter::reference)
template <typename SrcIter, typename DstIter, typename Transform>
Result<Ok,
       typename detail::TransformTraits<Transform, SrcIter>::result_err_type>
TransformAbortOnErr(SrcIter aIter, SrcIter aEnd, DstIter aDst,
                    Transform aTransform) {
  return TransformIfAbortOnErr(
      aIter, aEnd, aDst, [](const auto&) { return true; }, aTransform);
}

template <typename SrcRange, typename DstIter, typename Transform>
auto TransformAbortOnErr(SrcRange& aRange, DstIter aDst, Transform aTransform) {
  using std::begin;
  using std::end;
  return TransformIfAbortOnErr(
      begin(aRange), end(aRange), aDst, [](const auto&) { return true; },
      aTransform);
}

}  // namespace mozilla

#endif  // mozilla_Algorithm_h
