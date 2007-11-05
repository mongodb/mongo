/*=============================================================================
    Copyright (c) 2003-2005 Joel de Guzman
    Copyright (c) 2005 Dan Marsden

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_FILTER_VIEW_ITERATOR_EQUAL_TO_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_FILTER_VIEW_ITERATOR_EQUAL_TO_TRAITS_HPP

namespace boost { namespace fusion
{
    struct filter_view_iterator_tag;

    namespace meta
    {
        template<typename I1, typename I2>
        struct equal_to;

        template<typename Tag>
        struct equal_to_impl;

        template<>
        struct equal_to_impl<filter_view_iterator_tag>
        {
            template<typename I1, typename I2>
            struct apply
                : equal_to<typename I1::first_type, typename I2::first_type>
            {};
        };
    }
}}

#endif
