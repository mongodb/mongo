/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_TUPLE_VALUE_AT_TRAITS_IPP)
#define FUSION_SEQUENCE_DETAIL_TUPLE_VALUE_AT_TRAITS_IPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/mpl/at.hpp>

namespace boost { namespace fusion
{
    struct tuple_tag;

    namespace meta
    {
        template <typename Tag>
        struct value_at_impl;

        template <>
        struct value_at_impl<tuple_tag>
        {
            template <typename Tuple, int N>
            struct apply
            {
                typedef typename
                    mpl::at_c<FUSION_GET_TYPES(Tuple), N>::type
                type;
            };
        };
    }
}}

#endif
