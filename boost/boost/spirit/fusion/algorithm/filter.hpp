/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_FILTER_HPP)
#define FUSION_ALGORITHM_FILTER_HPP

#include <boost/spirit/fusion/sequence/filter_view.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename Pred>
        struct filter
        {
            typedef filter_view<Sequence, Pred> type;
        };
    }

    namespace function
    {
        struct filter
        {
            template <typename Sequence, typename Pred>
            struct apply : meta::filter<Sequence, Pred> {};

            template <typename Sequence, typename Pred>
            typename apply<Sequence const, Pred>::type
            operator()(Sequence const& seq, Pred) const
            {
                return filter_view<Sequence const, Pred>(seq);
            }

            template <typename Sequence, typename Pred>
            typename apply<Sequence, Pred>::type
            operator()(Sequence& seq, Pred) const
            {
                return filter_view<Sequence, Pred>(seq);
            }
        };
    }

    function::filter const filter = function::filter();
}}

#endif

