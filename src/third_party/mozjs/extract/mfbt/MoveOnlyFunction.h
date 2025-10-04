/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_MoveOnlyFunction_h
#define mozilla_MoveOnlyFunction_h

// Use stl-like empty propagation to avoid issues with wrapping closures which
// implicitly coerce to bool.
#define FU2_WITH_LIMITED_EMPTY_PROPAGATION

#include "function2/function2.hpp"

namespace mozilla {

/// A type like `std::function`, but with support for move-only callable
/// objects.
///
/// A similar type is proposed to be added to the standard library as
/// `std::move_only_function` in C++23.
///
/// Unlike `std::function`, the function signature may be given const or
/// reference qualifiers which will be applied to `operator()`. This can be used
/// to declare const qualified or move-only functions.
///
/// The implementation this definition depends on (function2) also has support
/// for callables with overload sets, however support for this was not exposed
/// to align better with the proposed `std::move_only_function`, which does not
/// support overload sets.
///
/// A custom typedef over `fu2::function_base` is used to control the size and
/// alignment of the inline storage to store 2 aligned pointers, and ensure the
/// type is compatible with `nsTArray`.
template <typename Signature>
using MoveOnlyFunction = fu2::function_base<
    /* IsOwning */ true,
    /* IsCopyable */ false,
    /* Capacity */ fu2::capacity_fixed<2 * sizeof(void*), alignof(void*)>,
    /* IsThrowing */ false,
    /* HasStrongExceptionGuarantee */ false,
    /* Signature */ Signature>;

}  // namespace mozilla

#endif  // mozilla_MoveOnlyFunction_h
