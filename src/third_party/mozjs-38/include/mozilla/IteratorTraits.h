/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Iterator traits to expose a value type and a difference type */

#ifndef mozilla_IteratorTraits_h
#define mozilla_IteratorTraits_h

#include <stddef.h>

namespace mozilla {

template<typename Iterator>
struct IteratorTraits
{
  typedef typename Iterator::ValueType ValueType;
  typedef typename Iterator::DifferenceType DifferenceType;
};

template<typename T>
struct IteratorTraits<T*>
{
  typedef T ValueType;
  typedef ptrdiff_t DifferenceType;
};

template<typename T>
struct IteratorTraits<const T*>
{
  typedef const T ValueType;
  typedef ptrdiff_t DifferenceType;
};

} // namespace mozilla

#endif // mozilla_IteratorTraits_h
