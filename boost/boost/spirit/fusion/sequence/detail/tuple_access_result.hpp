/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_TUPLE_ACCESS_RESULT_HPP)
#define FUSION_SEQUENCE_DETAIL_TUPLE_ACCESS_RESULT_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/mpl/at.hpp>
#include <boost/mpl/eval_if.hpp>

namespace boost { namespace fusion { namespace detail
{
    template <typename Tuple, int N>
    struct tuple_access_result
    {
        typedef mpl::at_c<FUSION_GET_TYPES(Tuple), N> element;
        typedef typename
            mpl::eval_if<
                is_const<Tuple>
              , cref_result<element>
              , ref_result<element>
            >::type
        type;
    };
}}}

#endif
