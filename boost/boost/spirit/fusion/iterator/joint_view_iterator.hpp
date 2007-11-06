/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_JOINT_VIEW_ITERATOR_HPP)
#define FUSION_ITERATOR_JOINT_VIEW_ITERATOR_HPP

#include <boost/spirit/fusion/iterator/as_fusion_iterator.hpp>
#include <boost/spirit/fusion/iterator/detail/iterator_base.hpp>
#include <boost/spirit/fusion/iterator/detail/joint_view_iterator/deref_traits.hpp>
#include <boost/spirit/fusion/iterator/detail/joint_view_iterator/next_traits.hpp>
#include <boost/spirit/fusion/iterator/detail/joint_view_iterator/value_traits.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>

namespace boost { namespace fusion
{
    struct joint_view_iterator_tag;

    template <typename First, typename Last, typename Concat>
    struct joint_view_iterator
        : iterator_base<joint_view_iterator<First, Last, Concat> >
    {
        typedef as_fusion_iterator<First> first_converter;
        typedef as_fusion_iterator<Last> last_converter;
        typedef as_fusion_iterator<Concat> concat_converter;

        typedef typename first_converter::type first_type;
        typedef typename last_converter::type last_type;
        typedef typename concat_converter::type concat_type;

        typedef joint_view_iterator_tag tag;
#if! BOOST_WORKAROUND(BOOST_MSVC,<=1300)
        BOOST_STATIC_ASSERT((!meta::equal_to<first_type, last_type>::value));
#endif
        joint_view_iterator(First const& first, Concat const& concat);

        first_type first;
        concat_type concat;
    };
    template <typename First, typename Last, typename Concat>
    joint_view_iterator<First,Last,Concat>::joint_view_iterator(First const& first, Concat const& concat)
    : first(first_converter::convert(first))
    , concat(concat_converter::convert(concat))
    {}


}}

#endif


