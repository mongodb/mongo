/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_TUPLE_ELEMENT_HPP)
#define FUSION_SEQUENCE_TUPLE_ELEMENT_HPP

#include <boost/spirit/fusion/sequence/value_at.hpp>
#include <utility>

namespace boost { namespace fusion
{
    ///////////////////////////////////////////////////////////////////////////
    //
    //  tuple_element metafunction
    //
    //      Given a constant integer N and a Sequence, returns the
    //      tuple element type at slot N. (N is a zero based index). Usage:
    //
    //          tuple_element<N, Sequence>::type
    //
    //  This metafunction is provided here for compatibility with the
    //  tuples TR1 specification. This metafunction forwards to
    //  meta::value_at_c<Sequence>.
    //
    ///////////////////////////////////////////////////////////////////////////
    template <int N, typename Sequence>
    struct tuple_element : meta::value_at_c<Sequence, N> {};

    template<typename T1, typename T2>
    struct tuple_element<0, std::pair<T1, T2> >
    {
        typedef T1 type;
    };

    template<typename T1, typename T2>
    struct tuple_element<1, std::pair<T1, T2> >
    {
        typedef T2 type;
    };
}}

#endif
