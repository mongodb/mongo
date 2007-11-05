/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_TYPE_SEQUENCE_ITERATOR_DEREF_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_TYPE_SEQUENCE_ITERATOR_DEREF_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/mpl/deref.hpp>

namespace boost { namespace fusion
{
    namespace type_sequence_iterator_detail
    {
        template <typename Iterator>
        struct deref_traits_impl
        {
            typedef typename mpl::deref<
                typename Iterator::iterator_type>::type
            type;

            static type
            call(Iterator);
        };

        template <typename Iterator>
        inline typename deref_traits_impl<Iterator>::type
        deref_traits_impl<Iterator>::call(Iterator)
        {
            FUSION_RETURN_DEFAULT_CONSTRUCTED;
        }
    }

    struct type_sequence_iterator_tag;

    namespace meta
    {
        template <typename Tag>
        struct deref_impl;

        template <>
        struct deref_impl<type_sequence_iterator_tag>
        {
            template <typename Iterator>
            struct apply
                : type_sequence_iterator_detail::deref_traits_impl<Iterator> {};
        };
    }
}}

#endif


