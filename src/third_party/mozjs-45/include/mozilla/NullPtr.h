/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implements a mozilla::IsNullPointer<T> type trait. */

#ifndef mozilla_NullPtr_h
#define mozilla_NullPtr_h

#include "mozilla/TypeTraits.h"

namespace mozilla {

/**
 * IsNullPointer<T>::value is true iff T is decltype(nullptr).
 *
 * Ideally this would be in TypeTraits.h, but C++11 omitted std::is_null_pointer
 * (fixed in C++14), so in the interests of easing a switch to <type_traits>,
 * this trait lives elsewhere.
 */
template<typename T>
struct IsNullPointer : FalseType {};

template<>
struct IsNullPointer<decltype(nullptr)> : TrueType {};

} // namespace mozilla

#endif /* mozilla_NullPtr_h */
