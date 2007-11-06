/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_FOLD_HPP)
#define FUSION_ALGORITHM_FOLD_HPP

#include <boost/spirit/fusion/algorithm/detail/fold.ipp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename State, typename F>
        struct fold
        {
            typedef typename
                detail::static_fold<
                    typename meta::begin<Sequence>::type
                  , typename meta::end<Sequence>::type
                  , State
                  , F
                >::type
            type;
        };
    }

    namespace function
    {
        struct fold
        {
            template <typename Sequence, typename State, typename F>
            struct apply : meta::fold<Sequence, State, F> {};

            template <typename Sequence, typename State, typename F>
            inline typename apply<Sequence const, State, F>::type
            operator()(Sequence const& seq, State const& state, F const& f) const
            {
                return detail::fold(
                    fusion::begin(seq)
                  , fusion::end(seq)
                  , state
                  , f
                  , is_same<
                        BOOST_DEDUCED_TYPENAME meta::begin<Sequence const>::type
                      , BOOST_DEDUCED_TYPENAME meta::end<Sequence const>::type>()
                );
            }

            template <typename Sequence, typename State, typename F>
            inline typename apply<Sequence, State, F>::type
            operator()(Sequence& seq, State const& state, F const& f) const
            {
                return detail::fold(
                    fusion::begin(seq)
                  , fusion::end(seq)
                  , state
                  , f
                  , is_same<
                        BOOST_DEDUCED_TYPENAME meta::begin<Sequence>::type
                      , BOOST_DEDUCED_TYPENAME meta::end<Sequence>::type>()
                );
            }
        };
    }

    function::fold const fold = function::fold();
}}

#endif

