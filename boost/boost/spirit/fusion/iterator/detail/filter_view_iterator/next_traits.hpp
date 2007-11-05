/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_FILTER_VIEW_ITERATOR_NEXT_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_FILTER_VIEW_ITERATOR_NEXT_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/algorithm/detail/find_if.ipp>
#include <boost/spirit/fusion/iterator/next.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/identity.hpp>

namespace boost { namespace fusion
{
    struct filter_view_iterator_tag;

    template <typename First, typename Last, typename Pred>
    struct filter_iterator;

    namespace filter_view_detail {
        template<typename Iterator>
        struct next_traits_impl {
            typedef typename Iterator::first_type first_type;
            typedef typename Iterator::last_type last_type;
            typedef typename Iterator::pred_type pred_type;

            typedef typename
                mpl::eval_if<
                    meta::equal_to<first_type, last_type>
                  , mpl::identity<last_type>
                  , meta::next<first_type>
                >::type
            next_type;

            typedef typename detail::static_find_if<
                next_type, last_type, pred_type>
            filter;

            typedef filter_iterator<
                typename filter::type, last_type, pred_type>
            type;

            static type
            call(Iterator const& i);
        };
        template<typename Iterator>
        typename next_traits_impl<Iterator>::type 
        next_traits_impl<Iterator>::call(Iterator const& i)
        {
            return type(filter::call(i.first));
        }
    }

    namespace meta
    {
        template <typename Tag>
        struct next_impl;

        template <>
        struct next_impl<filter_view_iterator_tag>
        {
            template <typename Iterator>
            struct apply : filter_view_detail::next_traits_impl<Iterator>
            {};
        };
    }
}}

#endif


