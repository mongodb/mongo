/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2005 Eric Niebler

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_CONS_ITERATOR_DEREF_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_CONS_ITERATOR_DEREF_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/type_traits/add_const.hpp>
#include <boost/type_traits/add_reference.hpp>

namespace boost { namespace fusion
{
    struct cons_iterator_tag;

    namespace cons_detail
    {
        template <typename Iterator>
        struct deref_traits_impl
        {
            typedef typename Iterator::cons_type cons_type;
            typedef typename cons_type::car_type value_type;

            typedef typename mpl::eval_if<
                is_const<cons_type>
              , add_reference<typename add_const<value_type>::type>
              , add_reference<value_type> >::type
            type;

            static type
            call(Iterator const& i)
            {
                return detail::ref(i.cons.car);
            }
        };
    }

    namespace meta
    {
        template <typename Tag>
        struct deref_impl;

        template <>
        struct deref_impl<cons_iterator_tag>
        {
            template <typename Iterator>
            struct apply : cons_detail::deref_traits_impl<Iterator> {};
        };
    }
}}

#endif


