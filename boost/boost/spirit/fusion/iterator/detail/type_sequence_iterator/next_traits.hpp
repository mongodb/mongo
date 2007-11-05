/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_TYPE_SEQUENCE_ITERATOR_NEXT_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_TYPE_SEQUENCE_ITERATOR_NEXT_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/mpl/next.hpp>

namespace boost { namespace fusion
{
    struct type_sequence_iterator_tag;

    template <typename Iterator>
    struct type_sequence_iterator;

    namespace type_sequence_detail {
        template <typename Iterator>
        struct next_traits_impl
        {
            typedef type_sequence_iterator<
                typename mpl::next<typename Iterator::iterator_type>::type
            > type;

            static type
            call(Iterator);
        };

        template <typename Iterator>
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
        struct next_impl<type_sequence_iterator_tag>
        {
            template <typename Iterator>
            struct apply : type_sequence_detail::next_traits_impl<Iterator>
            {};
        };
    }
}}

#endif


