/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt
    Copyright (c) 2005 Eric Niebler

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_DETAIL_ANY_IPP)
#define FUSION_ALGORITHM_DETAIL_ANY_IPP

#include <boost/mpl/bool.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>
#include <boost/spirit/fusion/iterator/deref.hpp>

namespace boost { namespace fusion { namespace detail
{
    template <typename First, typename Last, typename F>
    inline bool
    any(First const&, Last const&, F const&, mpl::true_)
    {
        return false;
    }

    template <typename First, typename Last, typename F>
    inline bool
    any(First const& first, Last const& last, F const& f, mpl::false_)
    {
        if(f(*first))
            return true;
        return detail::any(fusion::next(first), last, f
            , meta::equal_to<BOOST_DEDUCED_TYPENAME meta::next<First>::type, Last>());
    }
}}}

#endif

