/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_APPEND_VIEW_HPP)
#define FUSION_SEQUENCE_APPEND_VIEW_HPP

#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/sequence/joint_view.hpp>
#include <boost/spirit/fusion/sequence/single_view.hpp>

namespace boost { namespace fusion
{
    template <typename View, typename T>
    struct append_view : joint_view<View, single_view<T> >
    {
        append_view(View& view, typename detail::call_param<T>::type val)
            : joint_view<View, single_view<T> >(view, held)
            , held(val) {}
        single_view<T> held;
    };
}}

#endif


