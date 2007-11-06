/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_FIND_IF_HPP)
#define FUSION_ALGORITHM_FIND_IF_HPP

#include <boost/spirit/fusion/algorithm/detail/find_if.ipp>
#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename Pred>
        struct find_if
        {
            typedef typename
                detail::static_find_if<
                    typename meta::begin<Sequence>::type
                  , typename meta::end<Sequence>::type
                  , Pred
                >::type
            type;
        };
    }

    namespace function
    {
        struct find_if
        {
            template <typename Sequence, typename Pred>
            struct apply : meta::find_if<Sequence, Pred> {};

            template <typename Sequence, typename Pred>
            inline typename apply<Sequence const, Pred>::type
            operator()(Sequence const& seq, Pred) const
            {
                typedef
                    detail::static_find_if<
                        BOOST_DEDUCED_TYPENAME meta::begin<Sequence const>::type
                      , BOOST_DEDUCED_TYPENAME meta::end<Sequence const>::type
                      , Pred
                    >
                filter;

                return filter::call(fusion::begin(seq));
            }

            template <typename Sequence, typename Pred>
            inline typename apply<Sequence, Pred>::type
            operator()(Sequence& seq, Pred) const
            {
                typedef
                    detail::static_find_if<
                        BOOST_DEDUCED_TYPENAME meta::begin<Sequence>::type
                      , BOOST_DEDUCED_TYPENAME meta::end<Sequence>::type
                      , Pred
                    >
                filter;

                return filter::call(fusion::begin(seq));
            }
        };
    }

    function::find_if const find_if = function::find_if();
}}

#endif

