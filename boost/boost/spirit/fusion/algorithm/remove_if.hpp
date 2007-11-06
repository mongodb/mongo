/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_REMOVE_IF_HPP)
#define FUSION_ALGORITHM_REMOVE_IF_HPP

#include <boost/spirit/fusion/sequence/filter_view.hpp>
#include <boost/mpl/not.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename Pred>
        struct remove_if
        {
            typedef filter_view<Sequence, mpl::not_<Pred> > type;
        };
    }

    namespace function
    {
        struct remove_if
        {
            template <typename Sequence, typename Pred>
            struct apply : meta::remove_if<Sequence, Pred> {};

            template <typename Sequence, typename Pred>
            inline typename apply<Sequence const, Pred>::type
            operator()(Sequence const& seq, Pred) const
            {
                return filter_view<Sequence const, mpl::not_<Pred> >(seq);
            }

            template <typename Sequence, typename Pred>
            inline typename apply<Sequence, Pred>::type
            operator()(Sequence& seq, Pred) const
            {
                return filter_view<Sequence, mpl::not_<Pred> >(seq);
            }
        };
    }

    function::remove_if const remove_if = function::remove_if();
}}

#endif

