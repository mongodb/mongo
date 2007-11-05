/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_RANGE_HPP)
#define FUSION_SEQUENCE_RANGE_HPP

#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/sequence/detail/range_begin_end_traits.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_base.hpp>
#include <boost/spirit/fusion/iterator/as_fusion_iterator.hpp>

namespace boost { namespace fusion
{
    struct range_tag;

    template <typename First, typename Last>
    struct range : sequence_base<range<First, Last> >
    {
        typedef typename as_fusion_iterator<First>::type begin_type;
        typedef typename as_fusion_iterator<Last>::type end_type;
        typedef range_tag tag;

        range(First const& first, Last const& last)
            : first(as_fusion_iterator<First>::convert(first))
            , last(as_fusion_iterator<Last>::convert(last)) {}

        begin_type first;
        end_type last;
    };
}}

#endif


