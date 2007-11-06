/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_FILTER_VIEW_ITERATOR_HPP)
#define FUSION_ITERATOR_FILTER_VIEW_ITERATOR_HPP

#include <boost/spirit/fusion/iterator/as_fusion_iterator.hpp>
#include <boost/spirit/fusion/iterator/detail/iterator_base.hpp>
#include <boost/spirit/fusion/iterator/detail/filter_view_iterator/deref_traits.hpp>
#include <boost/spirit/fusion/iterator/detail/filter_view_iterator/next_traits.hpp>
#include <boost/spirit/fusion/iterator/detail/filter_view_iterator/value_traits.hpp>
#include <boost/spirit/fusion/iterator/detail/filter_view_iterator/equal_to_traits.hpp>
#include <boost/spirit/fusion/algorithm/detail/find_if.ipp>

namespace boost { namespace fusion
{
    struct filter_view_iterator_tag;

    template <typename First, typename Last, typename Pred>
    struct filter_iterator : iterator_base<filter_iterator<First, Last, Pred> >
    {
        typedef as_fusion_iterator<First> first_converter;
        typedef typename first_converter::type first_iter;
        typedef as_fusion_iterator<Last> last_converter;
        typedef typename last_converter::type last_iter;

        typedef filter_view_iterator_tag tag;
        typedef detail::static_find_if<first_iter, last_iter, Pred> filter;
        typedef typename filter::type first_type;
        typedef last_iter last_type;
        typedef Pred pred_type;

        filter_iterator(First const& first);

        first_type first;
    };

    template <typename First, typename Last, typename Pred>
    filter_iterator<First,Last,Pred>::filter_iterator(First const& first)
    :   first(filter::call(first_converter::convert(first))) 
    {}
}}

#endif


