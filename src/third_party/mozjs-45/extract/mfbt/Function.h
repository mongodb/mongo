/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A type-erased callable wrapper. */

#ifndef mozilla_Function_h
#define mozilla_Function_h

#include "mozilla/Attributes.h"  // for MOZ_IMPLICIT
#include "mozilla/Move.h"
#include "mozilla/UniquePtr.h"

// |Function<Signature>| is a wrapper that can hold any type of callable
// object that can be invoked in a way that's compatible with |Signature|.
// The standard "type erasure" technique is used to avoid the type of the
// wrapper depending on the concrete type of the wrapped callable.
//
// Supported callable types include non-member functions, static member
// functions, and function objects (that is to say, objects with an overloaded
// call operator; this includes C++11 lambdas). Member functions aren't
// directly supported; they first need to be wrapped into a function object
// using |std::mem_fn()| or an equivalent.
//
// |Signature| is a type of the form |ReturnType(Arguments...)|. Syntactically,
// this is a function type; it's not used in any way other than serving as a
// vehicle to encode the return and argument types into a single type.
//
// |Function| is default-constructible. A default-constructed instance is
// considered "empty". Invoking an empty instance is undefined behaviour.
// An empty instance can be populated with a callable by assigning to it.
//
// This class is intended to provide functionality similar to the C++11
// standard library class |std::function|.

namespace mozilla {

namespace detail {

template<typename ReturnType, typename... Arguments>
class FunctionImplBase
{
public:
  virtual ~FunctionImplBase() {}
  virtual ReturnType call(Arguments... aArguments) = 0;
};

// Normal Callable Object.
template <typename Callable, typename ReturnType, typename... Arguments>
class FunctionImpl : public FunctionImplBase<ReturnType, Arguments...>
{
  public:
    explicit FunctionImpl(const Callable& aCallable)
      : mCallable(aCallable) {}

    ReturnType call(Arguments... aArguments) override
    {
      return mCallable(Forward<Arguments>(aArguments)...);
    }
  private:
    Callable mCallable;
};

// Base class for passing pointer to member function.
template <typename Callable, typename ReturnType, typename... Arguments>
class MemberFunctionImplBase : public FunctionImplBase<ReturnType, Arguments...>
{
public:
  explicit MemberFunctionImplBase(const Callable& aCallable)
    : mCallable(aCallable) {}

  ReturnType call(Arguments... aArguments) override
  {
    return callInternal(Forward<Arguments>(aArguments)...);
  }
private:
  template<typename ThisType, typename... Args>
  ReturnType callInternal(ThisType* aThis, Args&&... aArguments)
  {
    return (aThis->*mCallable)(Forward<Args>(aArguments)...);
  }

  template<typename ThisType, typename... Args>
  ReturnType callInternal(ThisType&& aThis, Args&&... aArguments)
  {
    return (aThis.*mCallable)(Forward<Args>(aArguments)...);
  }
  Callable mCallable;
};

// For non-const member function specialization of FunctionImpl.
template <typename ThisType, typename... Args, typename ReturnType, typename... Arguments>
class FunctionImpl<ReturnType(ThisType::*)(Args...),
                   ReturnType, Arguments...>
  : public MemberFunctionImplBase<ReturnType(ThisType::*)(Args...),
                                  ReturnType, Arguments...>
{
public:
  explicit FunctionImpl(ReturnType(ThisType::*aMemberFunc)(Args...))
    : MemberFunctionImplBase<ReturnType(ThisType::*)(Args...),
                             ReturnType, Arguments...>(aMemberFunc)
  {}
};

// For const member function specialization of FunctionImpl.
template <typename ThisType, typename... Args, typename ReturnType, typename... Arguments>
class FunctionImpl<ReturnType(ThisType::*)(Args...) const,
                   ReturnType, Arguments...>
  : public MemberFunctionImplBase<ReturnType(ThisType::*)(Args...) const,
                                  ReturnType, Arguments...>
{
public:
  explicit FunctionImpl(ReturnType(ThisType::*aConstMemberFunc)(Args...) const)
    : MemberFunctionImplBase<ReturnType(ThisType::*)(Args...) const,
                             ReturnType, Arguments...>(aConstMemberFunc)
  {}
};

} // namespace detail

// The primary template is never defined. As |Signature| is required to be
// of the form |ReturnType(Arguments...)|, we only define a partial
// specialization that matches this form. This allows us to use |ReturnType|
// and |Arguments| in the definition of the specialization without having to
// introspect |Signature|.
template<typename Signature>
class Function;

template<typename ReturnType, typename... Arguments>
class Function<ReturnType(Arguments...)>
{
public:
  Function() {}

  // This constructor is implicit to match the interface of |std::function|.
  template <typename Callable>
  MOZ_IMPLICIT Function(const Callable& aCallable)
    : mImpl(MakeUnique<detail::FunctionImpl<Callable, ReturnType, Arguments...>>(aCallable))
  {}

  // Move constructor and move assingment operator.
  // These should be generated automatically, but MSVC doesn't do that yet.
  Function(Function&& aOther) : mImpl(Move(aOther.mImpl)) {}
  Function& operator=(Function&& aOther) {
    mImpl = Move(aOther.mImpl);
    return *this;
  }

  template <typename Callable>
  Function& operator=(const Callable& aCallable)
  {
    mImpl = MakeUnique<detail::FunctionImpl<Callable, ReturnType, Arguments...>>(aCallable);
    return *this;
  }

  template<typename... Args>
  ReturnType operator()(Args&&... aArguments) const
  {
    MOZ_ASSERT(mImpl);
    return mImpl->call(Forward<Args>(aArguments)...);
  }
private:
  // TODO: Consider implementing a small object optimization.
  UniquePtr<detail::FunctionImplBase<ReturnType, Arguments...>> mImpl;
};

} // namespace mozilla

#endif /* mozilla_Function_h */
