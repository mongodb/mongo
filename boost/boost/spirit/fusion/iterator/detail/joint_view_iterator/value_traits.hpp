/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_JOINT_VIEW_ITERATOR_VALUE_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_JOINT_VIEW_ITERATOR_VALUE_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/iterator/detail/adapt_value_traits.hpp>

namespace boost { namespace fusion
{
    struct joint_view_iterator_tag;

    namespace meta
    {
        template <typename Tag>
        struct value_impl;

        template <>
        struct value_impl<joint_view_iterator_tag>
            : detail::adapt_value_traits {};
    }
}}

#endif


