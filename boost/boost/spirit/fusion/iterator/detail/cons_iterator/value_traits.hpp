/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2005 Eric Niebler

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_CONS_ITERATOR_VALUE_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_CONS_ITERATOR_VALUE_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>

namespace boost { namespace fusion
{
    struct cons_iterator_tag;

    namespace cons_detail
    {
        template <typename Iterator>
        struct value_traits_impl
        {
            typedef typename Iterator::cons_type cons_type;
            typedef typename cons_type::car_type type;
        };
    }

    namespace meta
    {
        template <typename Tag>
        struct value_impl;

        template <>
        struct value_impl<cons_iterator_tag>
        {
            template <typename Iterator>
            struct apply : cons_detail::value_traits_impl<Iterator> {};
        };
    }

}}

#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)

namespace boost { namespace mpl
{
    template <typename Iterator>
    struct deref;

    template <typename Cons>
    struct deref<fusion::cons_iterator<Cons> >
        : fusion::cons_detail::value_traits_impl<fusion::cons_iterator<Cons> >
    {
    };
}}

#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

#endif


