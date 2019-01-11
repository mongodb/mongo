/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * A class storing one of two optional value types that supports in-place lazy
 * construction.
 */

#ifndef mozilla_MaybeOneOf_h
#define mozilla_MaybeOneOf_h

#include "mozilla/Assertions.h"
#include "mozilla/Move.h"
#include "mozilla/OperatorNewExtensions.h"
#include "mozilla/TemplateLib.h"

#include <new> // for placement new
#include <stddef.h> // for size_t

namespace mozilla {

/*
 * MaybeOneOf<T1, T2> is like Maybe, but it supports constructing either T1
 * or T2. When a MaybeOneOf<T1, T2> is constructed, it is |empty()|, i.e.,
 * no value has been constructed and no destructor will be called when the
 * MaybeOneOf<T1, T2> is destroyed. Upon calling |construct<T1>()| or
 * |construct<T2>()|, a T1 or T2 object will be constructed with the given
 * arguments and that object will be destroyed when the owning MaybeOneOf is
 * destroyed.
 *
 * Because MaybeOneOf must be aligned suitable to hold any value stored within
 * it, and because |alignas| requirements don't affect platform ABI with respect
 * to how parameters are laid out in memory, MaybeOneOf can't be used as the
 * type of a function parameter.  Pass MaybeOneOf to functions by pointer or
 * reference instead.
 */
template<class T1, class T2>
class MOZ_NON_PARAM MaybeOneOf
{
  static constexpr size_t StorageAlignment =
    tl::Max<alignof(T1), alignof(T2)>::value;
  static constexpr size_t StorageSize =
    tl::Max<sizeof(T1), sizeof(T2)>::value;

  alignas(StorageAlignment) unsigned char storage[StorageSize];

  // GCC fails due to -Werror=strict-aliasing if |storage| is directly cast to
  // T*.  Indirecting through these functions addresses the problem.
  void* data() { return storage; }
  const void* data() const { return storage; }

  enum State { None, SomeT1, SomeT2 } state;
  template <class T, class Ignored = void> struct Type2State {};

  template <class T>
  T& as()
  {
    MOZ_ASSERT(state == Type2State<T>::result);
    return *static_cast<T*>(data());
  }

  template <class T>
  const T& as() const
  {
    MOZ_ASSERT(state == Type2State<T>::result);
    return *static_cast<const T*>(data());
  }

public:
  MaybeOneOf() : state(None) {}
  ~MaybeOneOf() { destroyIfConstructed(); }

  MaybeOneOf(MaybeOneOf&& rhs)
    : state(None)
  {
    if (!rhs.empty()) {
      if (rhs.constructed<T1>()) {
        construct<T1>(Move(rhs.as<T1>()));
        rhs.as<T1>().~T1();
      } else {
        construct<T2>(Move(rhs.as<T2>()));
        rhs.as<T2>().~T2();
      }
      rhs.state = None;
    }
  }

  MaybeOneOf& operator=(MaybeOneOf&& rhs)
  {
    MOZ_ASSERT(this != &rhs, "Self-move is prohibited");
    this->~MaybeOneOf();
    new(this) MaybeOneOf(Move(rhs));
    return *this;
  }

  bool empty() const { return state == None; }

  template <class T>
  bool constructed() const { return state == Type2State<T>::result; }

  template <class T, class... Args>
  void construct(Args&&... aArgs)
  {
    MOZ_ASSERT(state == None);
    state = Type2State<T>::result;
    ::new (KnownNotNull, data()) T(Forward<Args>(aArgs)...);
  }

  template <class T>
  T& ref()
  {
    return as<T>();
  }

  template <class T>
  const T& ref() const
  {
    return as<T>();
  }

  void destroy()
  {
    MOZ_ASSERT(state == SomeT1 || state == SomeT2);
    if (state == SomeT1) {
      as<T1>().~T1();
    } else if (state == SomeT2) {
      as<T2>().~T2();
    }
    state = None;
  }

  void destroyIfConstructed()
  {
    if (!empty()) {
      destroy();
    }
  }

private:
  MaybeOneOf(const MaybeOneOf& aOther) = delete;
  const MaybeOneOf& operator=(const MaybeOneOf& aOther) = delete;
};

template <class T1, class T2>
template <class Ignored>
struct MaybeOneOf<T1, T2>::Type2State<T1, Ignored>
{
  typedef MaybeOneOf<T1, T2> Enclosing;
  static const typename Enclosing::State result = Enclosing::SomeT1;
};

template <class T1, class T2>
template <class Ignored>
struct MaybeOneOf<T1, T2>::Type2State<T2, Ignored>
{
  typedef MaybeOneOf<T1, T2> Enclosing;
  static const typename Enclosing::State result = Enclosing::SomeT2;
};

} // namespace mozilla

#endif /* mozilla_MaybeOneOf_h */
