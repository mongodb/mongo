/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_TYPE_SEQUENCE_ITERATOR_HPP)
#define FUSION_ITERATOR_TYPE_SEQUENCE_ITERATOR_HPP

#include <boost/spirit/fusion/iterator/detail/iterator_base.hpp>
#include <boost/spirit/fusion/iterator/detail/type_sequence_iterator/deref_traits.hpp>
#include <boost/spirit/fusion/iterator/detail/type_sequence_iterator/next_traits.hpp>
#include <boost/spirit/fusion/iterator/detail/type_sequence_iterator/value_traits.hpp>
#include <boost/type_traits/remove_const.hpp>

namespace boost { namespace fusion
{
    struct type_sequence_iterator_tag;

    template <typename Iterator>
    struct type_sequence_iterator
        : iterator_base<type_sequence_iterator<Iterator> >
    {
        typedef type_sequence_iterator_tag tag;
        typedef typename remove_const<Iterator>::type iterator_type;
    };
}}

#endif


