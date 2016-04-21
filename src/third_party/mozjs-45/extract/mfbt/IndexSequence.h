/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A utility for expanding a tuple into a variadic argument list.
 * Based on std::index_sequence. */

/**
 * Example usage:
 *
 * Problem:
 *
 *   You have a variadic function Foo:
 *
 *     template <typename... Args> void Foo(Args...);
 *
 *   And a variadic function Bar, which contains a tuple:
 *
 *     template <typename... Args>
 *     void Bar() {
 *       // ...
 *       Tuple<Args...> t;
 *     }
 *
 *   And inside Bar, you want to call Foo with the elements of the tuple as
 *   arguments to Foo.
 *
 *   You want to write:
 *
 *     Foo(Get<0>(t), Get<1>(t), ..., Get<N>(t))
 *
 *   but you can't literally write that, because N is different for different
 *   instantiations of Bar.
 *
 * Solution:
 *
 *   Write a helper function which takes the tuple, and an index sequence
 *   containing indices corresponding to the tuple indices.
 *
 *     template <typename... Args, size_t... Indices>
 *     void Helper(const Tuple<Args...>& t, IndexSequence<Indices>)
 *     {
 *       Foo(Get<Indices>(t)...);
 *     }
 *
 *   Assuming 'Indices...' are 0, 1, ..., N - 1, where N is the size of the
 *   tuple, pack expansion will expand the pack 'Get<Indices>(t)...' to
 *   'Get<0>(t), Get<1>(t), ..., Get<N>(t)'.
 *
 *   Finally, call the helper, creating the index sequence to pass in like so:
 *
 *     template <typename... Args>
 *     void Bar() {
 *       // ...
 *       Tuple<Args...> t;
 *       Helper(t, typename IndexSequenceFor<Args...>::Type());
 *     }
 */

#ifndef mozilla_IndexSequence_h
#define mozilla_IndexSequence_h

#include "mozilla/Attributes.h"

#include <stddef.h>

namespace mozilla {

/**
 * Represents a compile-time sequence of integer indices.
 */
template<size_t... Indices>
struct IndexSequence
{
  static MOZ_CONSTEXPR size_t Size() { return sizeof...(Indices); }
};

namespace detail {

// Helpers used by MakeIndexSequence.

template<size_t... Indices>
struct IndexTuple
{
  typedef IndexTuple<Indices..., sizeof...(Indices)> Next;
};

// Builds IndexTuple<0, 1, ..., N - 1>.
template<size_t N>
struct BuildIndexTuple
{
  typedef typename BuildIndexTuple<N - 1>::Type::Next Type;
};

template<>
struct BuildIndexTuple<0>
{
  typedef IndexTuple<> Type;
};

template<size_t N, typename IndexTuple>
struct MakeIndexSequenceImpl;

template<size_t N, size_t... Indices>
struct MakeIndexSequenceImpl<N, IndexTuple<Indices...>>
{
  typedef IndexSequence<Indices...> Type;
};

} // namespace detail

/**
 * A utility for building an IndexSequence of consecutive indices.
 * MakeIndexSequence<N>::Type evaluates to IndexSequence<0, 1, .., N - 1>.
 * Note: unlike std::make_index_sequence, this is not an alias template
 * to work around bugs in MSVC 2013.
 */
template<size_t N>
struct MakeIndexSequence
{
  typedef typename detail::MakeIndexSequenceImpl<N,
    typename detail::BuildIndexTuple<N>::Type>::Type Type;
};

/**
 * A utility for building an IndexSequence of consecutive indices
 * corresponding to a variadic argument list.
 * IndexSequenceFor<Types...> evaluates to IndexSequence<0, 1, ..., N - 1>
 * where N is the number of types in Types.
 * Note: unlike std::index_sequence_for, this is not an alias template
 * to work around bugs in MSVC 2013.
 */
template<typename... Types>
struct IndexSequenceFor
{
  typedef typename MakeIndexSequence<sizeof...(Types)>::Type Type;
};

} // namespace mozilla

#endif /* mozilla_IndexSequence_h */
