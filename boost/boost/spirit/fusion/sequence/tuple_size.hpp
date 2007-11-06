/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_TUPLE_SIZE_HPP)
#define FUSION_SEQUENCE_TUPLE_SIZE_HPP

#include <boost/spirit/fusion/sequence/size.hpp>
#include <boost/mpl/int.hpp>

#include <utility>

namespace boost { namespace fusion
{
    ///////////////////////////////////////////////////////////////////////////
    //
    //  tuple_size metafunction
    //
    //      Get the size of Sequence. Usage:
    //
    //          tuple_size<Sequence>::value
    //
    //  This metafunction is provided here for compatibility with the
    //  tuples TR1 specification. This metafunction forwards to
    //  meta::size<Sequence>.
    //
    ///////////////////////////////////////////////////////////////////////////
    template <typename Sequence>
    struct tuple_size : meta::size<Sequence> {};

    template<typename T1, typename T2>
    struct tuple_size<std::pair<T1, T2> > : boost::mpl::int_<2> { };
}}

#endif
