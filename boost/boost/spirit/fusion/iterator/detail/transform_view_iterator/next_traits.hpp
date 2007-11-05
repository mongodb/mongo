/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_TRANSFORM_VIEW_ITERATOR_NEXT_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_TRANSFORM_VIEW_ITERATOR_NEXT_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>

namespace boost { namespace fusion
{
    struct transform_view_iterator_tag;

    template <typename First, typename F>
    struct transform_view_iterator;
    
    namespace transform_view_detail {
        template <typename Iterator>
        struct next_traits_impl
        {
            typedef typename Iterator::first_type first_type;
            typedef typename meta::next<first_type>::type next_type;
            typedef typename Iterator::transform_type transform_type;
            typedef transform_view_iterator<next_type, transform_type> type;

            static type
            call(Iterator const& i);
        };
        template <typename Iterator>
        typename next_traits_impl<Iterator>::type
        next_traits_impl<Iterator>::call(Iterator const& i)
        {
            return type(fusion::next(i.first), i.f);
        }
    }

    namespace meta
    {
        template <typename Tag>
        struct next_impl;

        template <>
        struct next_impl<transform_view_iterator_tag>
        {
            template <typename Iterator>
            struct apply : transform_view_detail::next_traits_impl<Iterator>
            {};
        };
    }
}}

#endif


