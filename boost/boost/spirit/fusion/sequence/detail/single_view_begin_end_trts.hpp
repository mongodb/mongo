/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_SINGLE_VIEW_BEGIN_END_TRAITS_HPP)
#define FUSION_SEQUENCE_DETAIL_SINGLE_VIEW_BEGIN_END_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>

namespace boost { namespace fusion
{
    struct single_view_tag;

    template <typename T>
    struct single_view_iterator_end;

    template <typename T>
    struct single_view_iterator;

    namespace single_view_detail
    {
        template <typename Sequence>
        struct begin_traits_impl
        {
            typedef single_view_iterator<Sequence> type;

            static type
            call(Sequence& s);
        };

        template <typename Sequence>
        inline typename begin_traits_impl<Sequence>::type
        begin_traits_impl<Sequence>::call(Sequence& s)
        {
            return type(s);
        }

        template <typename Sequence>
        struct end_traits_impl
        {
            typedef single_view_iterator_end<Sequence> type;

            static type
            call(Sequence&);
        };

        template <typename Sequence>
        inline typename end_traits_impl<Sequence>::type
        end_traits_impl<Sequence>::call(Sequence&)
        {
            FUSION_RETURN_DEFAULT_CONSTRUCTED;
        }
    }

    namespace meta
    {
        template <typename Tag>
        struct begin_impl;

        template <>
        struct begin_impl<single_view_tag>
        {
            template <typename Sequence>
            struct apply : single_view_detail::begin_traits_impl<Sequence> {};
        };

        template <typename Tag>
        struct end_impl;

        template <>
        struct end_impl<single_view_tag>
        {
            template <typename Sequence>
            struct apply : single_view_detail::end_traits_impl<Sequence> {};
        };
    }
}}

namespace boost { namespace mpl
{
    template <typename Tag>
    struct begin_impl;

    template <typename Tag>
    struct end_impl;

    template <>
    struct begin_impl<fusion::single_view_tag>
        : fusion::meta::begin_impl<fusion::single_view_tag> {};

    template <>
    struct end_impl<fusion::single_view_tag>
        : fusion::meta::end_impl<fusion::single_view_tag> {};
}}

#endif


