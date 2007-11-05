/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_TRANSFORM_VIEW_ITERATOR_VALUE_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_TRANSFORM_VIEW_ITERATOR_VALUE_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/iterator/detail/adapt_value_traits.hpp>
#include <boost/spirit/fusion/iterator/value_of.hpp>

namespace boost { namespace fusion
{
    struct transform_view_iterator_tag;

    namespace meta
    {
        template <typename Tag>
        struct value_impl;

        template <>
        struct value_impl<transform_view_iterator_tag>
        {
            template <typename Iterator>
            struct apply
            {
                typedef typename
                    meta::value_of<typename Iterator::first_type>::type
                value_type;

                typedef typename Iterator::transform_type transform_type;
                typedef typename fusion_apply1<transform_type,value_type>::type type;
            };
        };
    }
}}

#endif


