/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_TUPLE_ITERATOR_DEREF_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_TUPLE_ITERATOR_DEREF_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_access_result.hpp>

namespace boost { namespace fusion
{
    struct tuple_iterator_tag;

    namespace detail
    {
        template <int N>
        struct tuple_access;
    }

    namespace tuple_iterator_detail
    {
        template <typename Iterator>
        struct deref_traits_impl
        {
            typedef FUSION_GET_INDEX(Iterator) index;
            typedef FUSION_GET_TUPLE(Iterator) tuple_;

            typedef BOOST_DEDUCED_TYPENAME
                boost::fusion::detail::tuple_access_result<
                    tuple_, FUSION_GET_VALUE(index)>::type
            type;

            static type
            call(Iterator const& i);
        };

        template <typename Iterator>
        inline typename deref_traits_impl<Iterator>::type
        deref_traits_impl<Iterator>::call(Iterator const& i)
        {
            return detail::tuple_access<FUSION_GET_VALUE(index)>
                ::get(detail::ref(i.get_tuple()));
        }
    }

    namespace meta
    {
        template <typename Tag>
        struct deref_impl;

        template <>
        struct deref_impl<tuple_iterator_tag>
        {
            template <typename Iterator>
            struct apply :
                tuple_iterator_detail::deref_traits_impl<Iterator> {};
        };
    }
}}

#endif
