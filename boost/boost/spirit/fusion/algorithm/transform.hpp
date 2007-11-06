/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_TRANSFORM_HPP)
#define FUSION_ALGORITHM_TRANSFORM_HPP

#include <boost/spirit/fusion/sequence/transform_view.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename F>
        struct transform
        {
            typedef transform_view<Sequence, F> type;
        };
    }

    namespace function
    {
        struct transform
        {
            template <typename Sequence, typename F>
            struct apply : meta::transform<Sequence, F> {};

            template <typename Sequence, typename F>
            typename apply<Sequence const, F>::type
            operator()(Sequence const& seq, F const& f) const
            {
                return transform_view<Sequence const, F>(seq, f);
            }

            template <typename Sequence, typename F>
            typename apply<Sequence, F>::type
            operator()(Sequence& seq, F const& f) const
            {
                return transform_view<Sequence, F>(seq, f);
            }
        };
    }

    function::transform const transform = function::transform();

}}

#endif

