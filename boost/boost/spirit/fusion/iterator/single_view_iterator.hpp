/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_SINGLE_VIEW_ITERATOR_HPP)
#define FUSION_ITERATOR_SINGLE_VIEW_ITERATOR_HPP

#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/sequence/detail/as_tuple_element.hpp>
#include <boost/spirit/fusion/iterator/detail/iterator_base.hpp>
#include <boost/spirit/fusion/iterator/detail/single_view_iterator/deref_traits.hpp>
#include <boost/spirit/fusion/iterator/detail/single_view_iterator/next_traits.hpp>
#include <boost/spirit/fusion/iterator/detail/single_view_iterator/value_traits.hpp>

namespace boost { namespace fusion
{
    struct single_view_iterator_tag;

    template <typename SingleView>
    struct single_view_iterator_end
        : iterator_base<single_view_iterator_end<SingleView> >
    {
        typedef single_view_iterator_tag tag;
    };

    template <typename SingleView>
    struct single_view_iterator
        : iterator_base<single_view_iterator<SingleView> >
    {
        typedef single_view_iterator_tag tag;
        typedef SingleView single_view_type;
        typedef typename add_reference<SingleView>::type reference_type;

        explicit single_view_iterator(reference_type view)
            : view(view) {}

        reference_type view;
    };
}}

#endif


