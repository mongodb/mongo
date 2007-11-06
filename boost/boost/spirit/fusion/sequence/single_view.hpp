/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_SINGLE_VIEW_HPP)
#define FUSION_SEQUENCE_SINGLE_VIEW_HPP

#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/sequence/detail/as_tuple_element.hpp>
#include <boost/spirit/fusion/iterator/single_view_iterator.hpp>
#include <boost/spirit/fusion/sequence/detail/single_view_begin_end_trts.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_base.hpp>

namespace boost { namespace fusion
{
    struct single_view_tag;

    template <typename T>
    struct single_view : sequence_base<single_view<T> >
    {
        typedef single_view_tag tag;
        typedef typename detail::as_tuple_element<T>::type value_type;

        single_view()
            : val() {}

        explicit single_view(typename detail::call_param<T>::type val)
            : val(val) {}

        value_type val;
    };
}}

#endif


