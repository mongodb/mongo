/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A variadic tuple class. */

#ifndef mozilla_Tuple_h
#define mozilla_Tuple_h

#include "mozilla/Move.h"
#include "mozilla/Pair.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/TypeTraits.h"

#include <stddef.h>
#include <utility>

namespace mozilla {

namespace detail {

/*
 * A helper class that allows passing around multiple variadic argument lists
 * by grouping them.
 */
template<typename... Ts>
struct Group;

/*
 * CheckConvertibility checks whether each type in a source pack of types
 * is convertible to the corresponding type in a target pack of types.
 *
 * It is intended to be invoked like this:
 *   CheckConvertibility<Group<SourceTypes...>, Group<TargetTypes...>>
 * 'Group' is used to separate types in the two packs (otherwise if we just
 * wrote 'CheckConvertibility<SourceTypes..., TargetTypes...', it couldn't
 * know where the first pack ends and the second begins).
 *
 * Note that we need to check explicitly that the two packs are of the same
 * size, because attempting to simultaneously expand two parameter packs
 * is an error (and it would be a hard error, because it wouldn't be in the
 * immediate context of the caller).
 */

template<typename Source, typename Target, bool SameSize>
struct CheckConvertibilityImpl;

template<typename Source, typename Target>
struct CheckConvertibilityImpl<Source, Target, false>
  : FalseType {};

template<typename... SourceTypes, typename... TargetTypes>
struct CheckConvertibilityImpl<Group<SourceTypes...>, Group<TargetTypes...>, true>
  : IntegralConstant<bool, tl::And<IsConvertible<SourceTypes, TargetTypes>::value...>::value> { };

template<typename Source, typename Target>
struct CheckConvertibility;

template<typename... SourceTypes, typename... TargetTypes>
struct CheckConvertibility<Group<SourceTypes...>, Group<TargetTypes...>>
  : CheckConvertibilityImpl<Group<SourceTypes...>, Group<TargetTypes...>,
        sizeof...(SourceTypes) == sizeof...(TargetTypes)> { };

/*
 * TupleImpl is a helper class used to implement mozilla::Tuple.
 * It represents one node in a recursive inheritance hierarchy.
 * 'Index' is the 0-based index of the tuple element stored in this node;
 * 'Elements...' are the types of the elements stored in this node and its
 * base classes.
 *
 * Example:
 *   Tuple<int, float, char> inherits from
 *   TupleImpl<0, int, float, char>, which stores the 'int' and inherits from
 *   TupleImpl<1, float, char>, which stores the 'float' and inherits from
 *   TupleImpl<2, char>, which stores the 'char' and inherits from
 *   TupleImpl<3>, which stores nothing and terminates the recursion.
 *
 * The purpose of the 'Index' parameter is to allow efficient index-based
 * access to a tuple element: given a tuple, and an index 'I' that we wish to
 * access, we can cast the tuple to the base which stores the I'th element
 * by performing template argument deduction against 'TupleImpl<I, E...>',
 * where 'I' is specified explicitly and 'E...' is deduced (this is what the
 * non-member 'Get<N>(t)' function does).
 *
 * This implementation strategy is borrowed from libstdc++'s std::tuple
 * implementation.
 */
template<std::size_t Index, typename... Elements>
struct TupleImpl;

/*
 * The base case of the inheritance recursion (and also the implementation
 * of an empty tuple).
 */
template<std::size_t Index>
struct TupleImpl<Index> {};

/*
 * One node of the recursive inheritance hierarchy. It stores the element at
 * index 'Index' of a tuple, of type 'HeadT', and inherits from the nodes
 * that store the remaining elements, of types 'TailT...'.
 */
template<std::size_t Index, typename HeadT, typename... TailT>
struct TupleImpl<Index, HeadT, TailT...>
  : public TupleImpl<Index + 1, TailT...>
{
  typedef TupleImpl<Index + 1, TailT...> Base;

  // Accessors for the head and the tail.
  // These are static, because the intended usage is for the caller to,
  // given a tuple, obtain the type B of the base class which stores the
  // element of interest, and then call B::Head(tuple) to access it.
  // (Tail() is mostly for internal use, but is exposed for consistency.)
  static HeadT& Head(TupleImpl& aTuple) { return aTuple.mHead; }
  static const HeadT& Head(const TupleImpl& aTuple) { return aTuple.mHead; }
  static Base& Tail(TupleImpl& aTuple) { return aTuple; }
  static const Base& Tail(const TupleImpl& aTuple) { return aTuple; }

  TupleImpl() : Base(), mHead() { }

  // Construct from const references to the elements.
  explicit TupleImpl(const HeadT& aHead, const TailT&... aTail)
    : Base(aTail...), mHead(aHead) { }

  // Construct from objects that are convertible to the elements.
  // This constructor is enabled only when the argument types are actually
  // convertible to the element types, otherwise it could become a better
  // match for certain invocations than the copy constructor.
  template <typename OtherHeadT, typename... OtherTailT,
            typename = typename EnableIf<
                CheckConvertibility<
                    Group<OtherHeadT, OtherTailT...>,
                    Group<HeadT, TailT...>>::value>::Type>
  explicit TupleImpl(OtherHeadT&& aHead, OtherTailT&&... aTail)
    : Base(Forward<OtherTailT>(aTail)...), mHead(Forward<OtherHeadT>(aHead)) { }

  // Copy and move constructors.
  // We'd like to use '= default' to implement these, but MSVC 2013's support
  // for '= default' is incomplete and this doesn't work.
  TupleImpl(const TupleImpl& aOther)
    : Base(Tail(aOther))
    , mHead(Head(aOther)) {}
  TupleImpl(TupleImpl&& aOther)
    : Base(Move(Tail(aOther)))
    , mHead(Forward<HeadT>(Head(aOther))) {}

  // Assign from a tuple whose elements are convertible to the elements
  // of this tuple.
  template <typename... OtherElements,
            typename = typename EnableIf<
                sizeof...(OtherElements) == sizeof...(TailT) + 1>::Type>
  TupleImpl& operator=(const TupleImpl<Index, OtherElements...>& aOther)
  {
    typedef TupleImpl<Index, OtherElements...> OtherT;
    Head(*this) = OtherT::Head(aOther);
    Tail(*this) = OtherT::Tail(aOther);
    return *this;
  }
  template <typename... OtherElements,
            typename = typename EnableIf<
                sizeof...(OtherElements) == sizeof...(TailT) + 1>::Type>
  TupleImpl& operator=(TupleImpl<Index, OtherElements...>&& aOther)
  {
    typedef TupleImpl<Index, OtherElements...> OtherT;
    Head(*this) = Move(OtherT::Head(aOther));
    Tail(*this) = Move(OtherT::Tail(aOther));
    return *this;
  }

  // Copy and move assignment operators.
  TupleImpl& operator=(const TupleImpl& aOther)
  {
    Head(*this) = Head(aOther);
    Tail(*this) = Tail(aOther);
    return *this;
  }
  TupleImpl& operator=(TupleImpl&& aOther)
  {
    Head(*this) = Move(Head(aOther));
    Tail(*this) = Move(Tail(aOther));
    return *this;
  }
private:
  HeadT mHead;  // The element stored at this index in the tuple.
};

} // namespace detail

/**
 * Tuple is a class that stores zero or more objects, whose types are specified
 * as template parameters. It can be thought of as a generalization of Pair,
 * (which can be thought of as a 2-tuple).
 *
 * Tuple allows index-based access to its elements (with the index having to be
 * known at compile time) via the non-member function 'Get<N>(tuple)'.
 */
template<typename... Elements>
class Tuple : public detail::TupleImpl<0, Elements...>
{
  typedef detail::TupleImpl<0, Elements...> Impl;
public:
  // The constructors and assignment operators here are simple wrappers
  // around those in TupleImpl.

  Tuple() : Impl() { }
  explicit Tuple(const Elements&... aElements) : Impl(aElements...) { }
  // Here, we can't just use 'typename... OtherElements' because MSVC will give
  // a warning "C4520: multiple default constructors specified" (even if no one
  // actually instantiates the constructor with an empty parameter pack -
  // that's probably a bug) and we compile with warnings-as-errors.
  template <typename OtherHead, typename... OtherTail,
            typename = typename EnableIf<
                detail::CheckConvertibility<
                    detail::Group<OtherHead, OtherTail...>,
                    detail::Group<Elements...>>::value>::Type>
  explicit Tuple(OtherHead&& aHead, OtherTail&&... aTail)
    : Impl(Forward<OtherHead>(aHead), Forward<OtherTail>(aTail)...) { }
  Tuple(const Tuple& aOther) : Impl(aOther) { }
  Tuple(Tuple&& aOther) : Impl(Move(aOther)) { }

  template <typename... OtherElements,
            typename = typename EnableIf<
                sizeof...(OtherElements) == sizeof...(Elements)>::Type>
  Tuple& operator=(const Tuple<OtherElements...>& aOther)
  {
    static_cast<Impl&>(*this) = aOther;
    return *this;
  }
  template <typename... OtherElements,
            typename = typename EnableIf<
                sizeof...(OtherElements) == sizeof...(Elements)>::Type>
  Tuple& operator=(Tuple<OtherElements...>&& aOther)
  {
    static_cast<Impl&>(*this) = Move(aOther);
    return *this;
  }
  Tuple& operator=(const Tuple& aOther)
  {
    static_cast<Impl&>(*this) = aOther;
    return *this;
  }
  Tuple& operator=(Tuple&& aOther)
  {
    static_cast<Impl&>(*this) = Move(aOther);
    return *this;
  }
};

/**
 * Specialization of Tuple for two elements.
 * This is created to support construction and assignment from a Pair or std::pair.
 */
template <typename A, typename B>
class Tuple<A, B> : public detail::TupleImpl<0, A, B>
{
  typedef detail::TupleImpl<0, A, B> Impl;

public:
  // The constructors and assignment operators here are simple wrappers
  // around those in TupleImpl.

  Tuple() : Impl() { }
  explicit Tuple(const A& aA, const B& aB) : Impl(aA, aB) { }
  template <typename AArg, typename BArg,
            typename = typename EnableIf<
                detail::CheckConvertibility<
                    detail::Group<AArg, BArg>,
                    detail::Group<A, B>>::value>::Type>
  explicit Tuple(AArg&& aA, BArg&& aB)
    : Impl(Forward<AArg>(aA), Forward<BArg>(aB)) { }
  Tuple(const Tuple& aOther) : Impl(aOther) { }
  Tuple(Tuple&& aOther) : Impl(Move(aOther)) { }
  explicit Tuple(const Pair<A, B>& aOther)
    : Impl(aOther.first(), aOther.second()) { }
  explicit Tuple(Pair<A, B>&& aOther) : Impl(Forward<A>(aOther.first()),
                                    Forward<B>(aOther.second())) { }
  explicit Tuple(const std::pair<A, B>& aOther)
    : Impl(aOther.first, aOther.second) { }
  explicit Tuple(std::pair<A, B>&& aOther) : Impl(Forward<A>(aOther.first),
                                    Forward<B>(aOther.second)) { }

  template <typename AArg, typename BArg>
  Tuple& operator=(const Tuple<AArg, BArg>& aOther)
  {
    static_cast<Impl&>(*this) = aOther;
    return *this;
  }
  template <typename AArg, typename BArg>
  Tuple& operator=(Tuple<AArg, BArg>&& aOther)
  {
    static_cast<Impl&>(*this) = Move(aOther);
    return *this;
  }
  Tuple& operator=(const Tuple& aOther)
  {
    static_cast<Impl&>(*this) = aOther;
    return *this;
  }
  Tuple& operator=(Tuple&& aOther)
  {
    static_cast<Impl&>(*this) = Move(aOther);
    return *this;
  }
  template <typename AArg, typename BArg>
  Tuple& operator=(const Pair<AArg, BArg>& aOther)
  {
    Impl::Head(*this) = aOther.first();
    Impl::Tail(*this).Head(*this) = aOther.second();
    return *this;
  }
  template <typename AArg, typename BArg>
  Tuple& operator=(Pair<AArg, BArg>&& aOther)
  {
    Impl::Head(*this) = Forward<AArg>(aOther.first());
    Impl::Tail(*this).Head(*this) = Forward<BArg>(aOther.second());
    return *this;
  }
  template <typename AArg, typename BArg>
  Tuple& operator=(const std::pair<AArg, BArg>& aOther)
  {
    Impl::Head(*this) = aOther.first;
    Impl::Tail(*this).Head(*this) = aOther.second;
    return *this;
  }
  template <typename AArg, typename BArg>
  Tuple& operator=(std::pair<AArg, BArg>&& aOther)
  {
    Impl::Head(*this) = Forward<AArg>(aOther.first);
    Impl::Tail(*this).Head(*this) = Forward<BArg>(aOther.second);
    return *this;
  }
};

/**
 * Specialization of Tuple for zero arguments.
 * This is necessary because if the primary template were instantiated with
 * an empty parameter pack, the 'Tuple(Elements...)' constructors would
 * become illegal overloads of the default constructor.
 */
template <>
class Tuple<> {};

namespace detail {

/*
 * Helper functions for implementing Get<N>(tuple).
 * These functions take a TupleImpl<Index, Elements...>, with Index being
 * explicitly specified, and Elements being deduced. By passing a Tuple
 * object as argument, template argument deduction will do its magic and
 * cast the tuple to the base class which stores the element at Index.
 */

// Const reference version.
template<std::size_t Index, typename... Elements>
auto TupleGetHelper(TupleImpl<Index, Elements...>& aTuple)
    -> decltype(TupleImpl<Index, Elements...>::Head(aTuple))
{
  return TupleImpl<Index, Elements...>::Head(aTuple);
}

// Non-const reference version.
template<std::size_t Index, typename... Elements>
auto TupleGetHelper(const TupleImpl<Index, Elements...>& aTuple)
    -> decltype(TupleImpl<Index, Elements...>::Head(aTuple))
{
  return TupleImpl<Index, Elements...>::Head(aTuple);
}

} // namespace detail

/**
 * Index-based access to an element of a tuple.
 * The syntax is Get<Index>(tuple). The index is zero-based.
 *
 * Example:
 *
 * Tuple<int, float, char> t;
 * ...
 * float f = Get<1>(t);
 */

// Non-const reference version.
template<std::size_t Index, typename... Elements>
auto Get(Tuple<Elements...>& aTuple)
    -> decltype(detail::TupleGetHelper<Index>(aTuple))
{
  return detail::TupleGetHelper<Index>(aTuple);
}

// Const reference version.
template<std::size_t Index, typename... Elements>
auto Get(const Tuple<Elements...>& aTuple)
    -> decltype(detail::TupleGetHelper<Index>(aTuple))
{
  return detail::TupleGetHelper<Index>(aTuple);
}

// Rvalue reference version.
template<std::size_t Index, typename... Elements>
auto Get(Tuple<Elements...>&& aTuple)
    -> decltype(Move(mozilla::Get<Index>(aTuple)))
{
  // We need a 'mozilla::' qualification here to avoid
  // name lookup only finding the current function.
  return Move(mozilla::Get<Index>(aTuple));
}

/**
 * A convenience function for constructing a tuple out of a sequence of
 * values without specifying the type of the tuple.
 * The type of the tuple is deduced from the types of its elements.
 *
 * Example:
 *
 * auto tuple = MakeTuple(42, 0.5f, 'c');  // has type Tuple<int, float, char>
 */
template<typename... Elements>
inline Tuple<typename Decay<Elements>::Type...>
MakeTuple(Elements&&... aElements)
{
  return Tuple<typename Decay<Elements>::Type...>(Forward<Elements>(aElements)...);
}

/**
 * A convenience function for constructing a tuple of references to a
 * sequence of variables. Since assignments to the elements of the tuple
 * "go through" to the referenced variables, this can be used to "unpack"
 * a tuple into individual variables.
 *
 * Example:
 *
 * int i;
 * float f;
 * char c;
 * Tie(i, f, c) = FunctionThatReturnsATuple();
 */
template<typename... Elements>
inline Tuple<Elements&...>
Tie(Elements&... aVariables)
{
  return Tuple<Elements&...>(aVariables...);
}

} // namespace mozilla

#endif /* mozilla_Tuple_h */
