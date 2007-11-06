/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_TUPLE_AT_TRAITS_HPP)
#define FUSION_SEQUENCE_DETAIL_TUPLE_AT_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_access_result.hpp>

namespace boost { namespace fusion
{
    struct tuple_tag;

    namespace detail
    {
        template <int N>
        struct tuple_access;
    }

    namespace detail
    {
        template <typename Sequence, int N>
        struct tuple_at_impl
        {
            typedef BOOST_DEDUCED_TYPENAME
                boost::fusion::detail::tuple_access_result<Sequence, N>::type
            type;

            static type
            call(Sequence& t);
        };

        template <typename Sequence, int N>
        inline typename tuple_at_impl<Sequence, N>::type
        tuple_at_impl<Sequence, N>::call(Sequence& t)
        {
            return detail::tuple_access<N>::get(t);
        }
    }

    namespace meta
    {
        template <typename Tag>
        struct at_impl;

        template <>
        struct at_impl<tuple_tag>
        {
            template <typename Sequence, int N>
            struct apply : detail::tuple_at_impl<Sequence, N> {};
        };
    }
}}

#endif
