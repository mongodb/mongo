/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_RANGE_BEGIN_END_TRAITS_HPP)
#define FUSION_SEQUENCE_DETAIL_RANGE_BEGIN_END_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>

namespace boost { namespace fusion
{
    struct range_tag;

    template <typename First, typename Last>
    struct range;

    template <typename Tag>
    struct begin_impl;

    namespace range_detail
    {
        template <typename Sequence>
        struct begin_traits_impl
        {
            typedef typename Sequence::begin_type type;

            static type
            call(Sequence const& s);
        };

        template <typename Sequence>
        inline typename begin_traits_impl<Sequence>::type
        begin_traits_impl<Sequence>::call(Sequence const& s)
        {
            return s.first;
        }

        template <typename Sequence>
        struct end_traits_impl
        {
            typedef typename Sequence::end_type type;

            static type
            call(Sequence const& s);
        };

        template <typename Sequence>
        inline typename end_traits_impl<Sequence>::type
        end_traits_impl<Sequence>::call(Sequence const& s)
        {
            return s.last;
        }
    }

    namespace meta
    {
        template <typename Tag>
        struct begin_impl;

        template <>
        struct begin_impl<range_tag>
        {
            template <typename Sequence>
            struct apply : range_detail::begin_traits_impl<Sequence>
            {};
        };

        template <typename Tag>
        struct end_impl;

        template <>
        struct end_impl<range_tag>
        {
            template <typename Sequence>
            struct apply : range_detail::end_traits_impl<Sequence>
            {};
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
    struct begin_impl<fusion::range_tag>
        : fusion::meta::begin_impl<fusion::range_tag> {};

    template <>
    struct end_impl<fusion::range_tag>
        : fusion::meta::end_impl<fusion::range_tag> {};
}}

#endif


