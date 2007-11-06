/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_DETAIL_FOLD_IPP)
#define FUSION_ALGORITHM_DETAIL_FOLD_IPP

#include <boost/mpl/bool.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>
#include <boost/spirit/fusion/iterator/value_of.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>

namespace boost { namespace fusion { namespace detail
{
    template <typename Iterator, typename State, typename F>
    struct fold_apply
    {
        typedef typename fusion_apply2<F,
            typename meta::value_of<Iterator>::type, State
        >::type type;
    };

    template <typename First, typename Last, typename State, typename F>
    struct static_fold;

    template <typename First, typename Last, typename State, typename F>
    struct next_result_of_fold
    {
        typedef typename
            static_fold<
                typename meta::next<First>::type
              , Last
              , typename fold_apply<First, State, F>::type
              , F
            >::type
        type;
    };

    template <typename First, typename Last, typename State, typename F>
    struct static_fold
    {
        typedef typename
            mpl::if_<
                is_same<First, Last>
              , mpl::identity<State>
              , next_result_of_fold<First, Last, State, F>
            >::type
        result;

        typedef typename result::type type;
    };

    // terminal case
    template <typename First, typename Last, typename State, typename F>
    inline State const&
    fold(First const&, Last const&, State const& state, F const&, mpl::true_)
    {
        return state;
    }

    // non-terminal case
    template <typename First, typename Last, typename State, typename F>
    inline typename static_fold<First, Last, State, F>::type
    fold(
        First const& first
      , Last const& last
      , State const& state
      , F const& f
      , mpl::false_)
    {
        return detail::fold(
            fusion::next(first)
          , last
          , f(*first, state)
          , f
          , is_same<BOOST_DEDUCED_TYPENAME meta::next<First>::type, Last>()
        );
    }
}}}

#endif

