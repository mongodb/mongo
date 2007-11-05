/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_SINGLE_VIEW_ITERATOR_DEREF_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_SINGLE_VIEW_ITERATOR_DEREF_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/mpl/identity.hpp>

namespace boost { namespace fusion
{
    namespace detail
    {
        template <typename SingleView>
        struct single_view_access_result
        {
            typedef typename
                mpl::eval_if<
                    is_const<SingleView>
                  , cref_result<mpl::identity<FUSION_GET_VALUE_TYPE(SingleView)> >
                  , ref_result<mpl::identity<FUSION_GET_VALUE_TYPE(SingleView)> >
                >::type
            type;
        };
    }

    namespace single_view_iterator_detail
    {
        template <typename Iterator>
        struct deref_traits_impl
        {
            typedef typename Iterator::single_view_type single_view_type;
            typedef typename detail::single_view_access_result<
                single_view_type>::type 
            type;

            static type
            call(Iterator const& i);
        };

        template <typename Iterator>
        inline typename deref_traits_impl<Iterator>::type
        deref_traits_impl<Iterator>::call(Iterator const& i)
        {
            return detail::ref(i.view.val);
        }
    }

    struct single_view_iterator_tag;

    namespace meta
    {
        template <typename Tag>
        struct deref_impl;

        template <>
        struct deref_impl<single_view_iterator_tag>
        {
            template <typename Iterator>
            struct apply : single_view_iterator_detail::deref_traits_impl<Iterator> {};
        };
    }
}}

#endif


