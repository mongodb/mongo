/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_SINGLE_VIEW_ITERATOR_NEXT_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_SINGLE_VIEW_ITERATOR_NEXT_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>

namespace boost { namespace fusion
{
    struct single_view_iterator_tag;

    template <typename SingleView>
    struct single_view_iterator_end;

    template <typename SingleView>
    struct single_view_iterator;

    namespace single_view_detail 
    {
        template<typename Iterator>
        struct next_traits_impl 
        {
            typedef single_view_iterator_end<
                typename Iterator::single_view_type>
            type;

            static type
            call(Iterator);
        };

        template<typename Iterator>
        typename next_traits_impl<Iterator>::type
        next_traits_impl<Iterator>::call(Iterator)
        {
            FUSION_RETURN_DEFAULT_CONSTRUCTED;
        }
    }

    namespace meta
    {
        template <typename Tag>
        struct next_impl;

        template <>
        struct next_impl<single_view_iterator_tag>
        {
            template <typename Iterator>
            struct apply : single_view_detail::next_traits_impl<Iterator>
            {};
        };
    }
}}

#endif


