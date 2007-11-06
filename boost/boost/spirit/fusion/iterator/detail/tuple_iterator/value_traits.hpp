/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_TUPLE_ITERATOR_VALUE_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_TUPLE_ITERATOR_VALUE_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/sequence/tuple_element.hpp>

namespace boost { namespace fusion
{
    struct tuple_iterator_tag;

    namespace detail
    {
        template <int N>
        struct tuple_access;
    }

    namespace detail
    {
        template <typename Iterator>
        struct tuple_iterator_value_traits_impl
        {
            typedef FUSION_GET_INDEX(Iterator) index;
            typedef FUSION_GET_TUPLE(Iterator) tuple_;

            typedef BOOST_DEDUCED_TYPENAME
                tuple_element<
                    FUSION_GET_VALUE(index), tuple_>::type
            type;
        };
    }

    namespace meta
    {
        template <typename Tag>
        struct value_impl;

        template <>
        struct value_impl<tuple_iterator_tag>
        {
            template <typename Iterator>
            struct apply : detail::tuple_iterator_value_traits_impl<Iterator> {};
        };
    }
}}

#endif
