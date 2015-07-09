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

// If we had C++11 template aliases, we could just use this:
//
//   template <typename T,
//             size_t MinInlineCapacity = 0,
//             class AllocPolicy = TempAllocPolicy>
//   using Vector = mozilla::Vector<T, MinInlineCapacity, AllocPolicy>;
//
// ...and get rid of all the CRTP madness in mozilla::Vector(Base).  But we
// can't because compiler support's not up to snuff.  (Template aliases are in
// gcc 4.7 and clang 3.0 and are expected to be in MSVC 2013.)  Instead, have a
// completely separate class inheriting from mozilla::Vector, and throw CRTP at
// the problem til things work.
//
// This workaround presents a couple issues.  First, because js::Vector is a
// distinct type from mozilla::Vector, overload resolution, method calls, etc.
// are affected.  *Hopefully* this won't be too bad in practice.  (A bunch of
// places had to be fixed when mozilla::Vector was introduced, but it wasn't a
// crazy number.)  Second, mozilla::Vector's interface has to be made subclass-
// ready via CRTP -- or rather, via mozilla::VectorBase, which basically no one
// should use.  :-)  Third, we have to redefine the constructors and the non-
// inherited operators.  Blech.  Happily there aren't too many of these, so it
// isn't the end of the world.

template <typename T,
          size_t MinInlineCapacity = 0,
          class AllocPolicy = TempAllocPolicy>
class Vector
  : public mozilla::VectorBase<T,
                               MinInlineCapacity,
                               AllocPolicy,
                               Vector<T, MinInlineCapacity, AllocPolicy> >
{
    typedef typename mozilla::VectorBase<T, MinInlineCapacity, AllocPolicy, Vector> Base;

  public:
    explicit Vector(AllocPolicy alloc = AllocPolicy()) : Base(alloc) {}
    Vector(Vector&& vec) : Base(mozilla::Move(vec)) {}
    Vector& operator=(Vector&& vec) {
        return Base::operator=(mozilla::Move(vec));
    }
};

} // namespace js

#endif /* js_Vector_h */
