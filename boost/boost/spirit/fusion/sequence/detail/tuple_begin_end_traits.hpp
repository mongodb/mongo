/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_TUPLE_BEGIN_END_TRAITS_HPP)
#define FUSION_SEQUENCE_DETAIL_TUPLE_BEGIN_END_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>

namespace boost { namespace fusion
{
    struct tuple_tag;

    template <int N, typename Tuple>
    struct tuple_iterator;

    namespace tuple_detail
    {
        template <typename Tuple>
        struct begin_traits_impl
        {
            typedef tuple_iterator<0, Tuple> type;

            static type
            call(Tuple& t);
        };

        template <typename Tuple>
        inline typename begin_traits_impl<Tuple>::type
        begin_traits_impl<Tuple>::call(Tuple& t)
        {
            return type(t);
        }

        template <typename Tuple>
        struct end_traits_impl
        {
            typedef typename Tuple::size size;
            typedef tuple_iterator<FUSION_GET_VALUE(size), Tuple> type;

            static type
            call(Tuple& t);
        };

        template <typename Tuple>
        inline typename end_traits_impl<Tuple>::type
        end_traits_impl<Tuple>::call(Tuple& t)
        {
            return type(t);
        }
    }

    namespace meta
    {
        template <typename Tag>
        struct begin_impl;

        template <>
        struct begin_impl<tuple_tag>
        {
            template <typename Tuple>
            struct apply : tuple_detail::begin_traits_impl<Tuple> {};
        };

        template <typename Tag>
        struct end_impl;

        template <>
        struct end_impl<tuple_tag>
        {
            template <typename Tuple>
            struct apply : tuple_detail::end_traits_impl<Tuple> {};
        };
    }
}}

#include <boost/mpl/begin_end.hpp>
#include <boost/mpl/clear.hpp>
#include <boost/mpl/insert.hpp>
#include <boost/mpl/empty.hpp>
#include <boost/spirit/fusion/sequence/tuple_forward.hpp>

namespace boost { namespace fusion { namespace meta
{
    template <typename Sequence>
    struct generate;

    template <typename Sequence, typename T>
    struct push_front;
}}}

namespace boost { namespace mpl
{
    // these mpl traits really ought to be placed somewhere else

    template <typename Tag>
    struct begin_impl;

    template <>
    struct begin_impl<fusion::tuple_tag>
        : fusion::meta::begin_impl<fusion::tuple_tag> {};

    template <typename Tag>
    struct end_impl;

    template <>
    struct end_impl<fusion::tuple_tag>
        : fusion::meta::end_impl<fusion::tuple_tag> {};

    template <typename Tag>
    struct clear_impl;

    template <>
    struct clear_impl<fusion::tuple_tag>
    {
        template <typename Tuple>
        struct apply
        {
            typedef fusion::tuple<> type;
        };
    };

    template <typename Tag>
    struct push_front_impl;

    template <>
    struct push_front_impl<fusion::tuple_tag>
    {
        template <typename Tuple, typename T>
        struct apply
        {
            typedef typename fusion::meta::push_front<Tuple, T> func1_;
            typedef typename fusion::meta::generate<FUSION_GET_TYPE(func1_)>::type
            type;
        };
    };
}}

#endif
