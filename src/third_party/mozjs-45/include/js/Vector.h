/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Vector_h
#define js_Vector_h

#include "mozilla/Vector.h"

/* Silence dire "bugs in previous versions of MSVC have been fixed" warnings */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4345)
#endif

namespace js {

class TempAllocPolicy;

namespace detail {

template <typename T>
struct TypeIsGCThing : mozilla::FalseType
{};

// Uncomment this once we actually can assert it:
//template <>
//struct TypeIsGCThing<JS::Value> : mozilla::TrueType
//{};

} // namespace detail

template <typename T,
          size_t MinInlineCapacity = 0,
          class AllocPolicy = TempAllocPolicy
// 1800 is MSVC2013.  Optimistically assume MSVC2015 (1900) is fixed.
// If you're porting to MSVC2015 and this doesn't work, extend the
// condition to encompass that additional version (but *do* keep the
// version-check so we know when MSVC's fixed).
#if !defined(_MSC_VER) || (1800 <= _MSC_VER && _MSC_VER <= 1800)
         // Don't use this with JS::Value!  Use JS::AutoValueVector instead.
         , typename = typename mozilla::EnableIf<!detail::TypeIsGCThing<T>::value>::Type
#endif
         >
using Vector = mozilla::Vector<T, MinInlineCapacity, AllocPolicy>;

} // namespace js

#endif /* js_Vector_h */
