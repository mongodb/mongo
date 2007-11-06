/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt
    Copyright (c) 2005 Eric Niebler

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_CONS_BEGIN_END_TRAITS_HPP)
#define FUSION_SEQUENCE_DETAIL_CONS_BEGIN_END_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/mpl/if.hpp>
#include <boost/type_traits/is_const.hpp>

namespace boost { namespace fusion
{
    struct nil;

    struct cons_tag;

    template <typename Car, typename Cdr>
    struct cons;

    template <typename Cons>
    struct cons_iterator;

    namespace cons_detail
    {
        template <typename Cons>
        struct begin_traits_impl
        {
            typedef cons_iterator<Cons> type;

            static type
            call(Cons& t)
            {
                return type(t);
            }
        };

        template <typename Cons>
        struct end_traits_impl
        {
            typedef cons_iterator<
                typename mpl::if_<is_const<Cons>, nil const, nil>::type>
            type;

            static type
            call(Cons&)
            {
                FUSION_RETURN_DEFAULT_CONSTRUCTED;
            }
        };
    }

    namespace meta
    {
        template <typename Tag>
        struct begin_impl;

        template <>
        struct begin_impl<cons_tag>
        {
            template <typename Sequence>
            struct apply : cons_detail::begin_traits_impl<Sequence>
            {};
        };

        template <typename Tag>
        struct end_impl;

        template <>
        struct end_impl<cons_tag>
        {
            template <typename Sequence>
            struct apply : cons_detail::end_traits_impl<Sequence>
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
    struct begin_impl<fusion::cons_tag>
        : fusion::meta::begin_impl<fusion::cons_tag> {};

    template <>
    struct end_impl<fusion::cons_tag>
        : fusion::meta::end_impl<fusion::cons_tag> {};
}}

#endif
