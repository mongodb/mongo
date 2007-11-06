/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt
    Copyright (c) 2005 Eric Niebler

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_CONS_ITERATOR_NEXT_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_CONS_ITERATOR_NEXT_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/identity.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/type_traits/add_const.hpp>

namespace boost { namespace fusion
{
    struct cons_iterator_tag;

    template <typename Cons>
    struct cons_iterator;

    namespace cons_detail
    {
        template <typename Iterator>
        struct next_traits_impl
        {
            typedef typename Iterator::cons_type cons_type;
            typedef typename cons_type::cdr_type cdr_type;

            typedef cons_iterator<
                typename mpl::eval_if<
                    is_const<cons_type>
                  , add_const<cdr_type>
                  , mpl::identity<cdr_type>
                >::type>
            type;

            static type
            call(Iterator const& i)
            {
                return type(detail::ref(i.cons.cdr));
            }
        };
    }

    namespace meta
    {
        template <typename Tag>
        struct next_impl;

        template <>
        struct next_impl<cons_iterator_tag>
        {
            template <typename Iterator>
            struct apply : cons_detail::next_traits_impl<Iterator> {};
        };
    }
}}

#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)

namespace boost { namespace mpl
{
    template <typename Iterator>
    struct next;

    template <typename Cons>
    struct next<fusion::cons_iterator<Cons> >
        : fusion::cons_detail::next_traits_impl<fusion::cons_iterator<Cons> >
    {
    };
}}

#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

#endif


