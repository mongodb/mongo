/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Vector_h
#define js_Vector_h

#include "mozilla/Vector.h"

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
          class AllocPolicy = TempAllocPolicy,
         // Don't use this with JS::Value!  Use JS::AutoValueVector instead.
         typename = typename mozilla::EnableIf<!detail::TypeIsGCThing<T>::value>::Type
         >
using Vector = mozilla::Vector<T, MinInlineCapacity, AllocPolicy>;

} // namespace js

#endif /* js_Vector_h */
