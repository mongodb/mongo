/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_TRANSFORM_VIEW_BEGIN_END_TRAITS_HPP)
#define FUSION_SEQUENCE_DETAIL_TRANSFORM_VIEW_BEGIN_END_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>

namespace boost { namespace fusion
{
    struct transform_view_tag;

    template <typename Sequence, typename F>
    struct transform_view;

    template <typename First, typename F>
    struct transform_view_iterator;

    namespace meta
    {
        template <typename Tag>
        struct begin_impl;

        template <>
        struct begin_impl<transform_view_tag>
        {
            template <typename Sequence>
            struct apply
            {
                typedef typename Sequence::first_type first_type;
                typedef typename Sequence::transform_type transform_type;
                typedef transform_view_iterator<first_type, transform_type> type;

                static type
                call(Sequence& s)
                {
                    return type(s.first, s.f);
                }
            };
        };

        template <typename Tag>
        struct end_impl;

        template <>
        struct end_impl<transform_view_tag>
        {
            template <typename Sequence>
            struct apply
            {
                typedef typename Sequence::last_type last_type;
                typedef typename Sequence::transform_type transform_type;
                typedef transform_view_iterator<last_type, transform_type> type;

                static type
                call(Sequence& s)
                {
                    return type(s.last, s.f);
                }
            };
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
    struct begin_impl<fusion::transform_view_tag>
        : fusion::meta::begin_impl<fusion::transform_view_tag> {};

    template <>
    struct end_impl<fusion::transform_view_tag>
        : fusion::meta::end_impl<fusion::transform_view_tag> {};
}}

#endif


