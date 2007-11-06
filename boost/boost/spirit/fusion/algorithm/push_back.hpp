/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_PUSH_BACK_HPP)
#define FUSION_ALGORITHM_PUSH_BACK_HPP

#include <boost/spirit/fusion/sequence/append_view.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename T>
        struct push_back
        {
            typedef append_view<Sequence, T> type;
        };
    }

    namespace function
    {
        struct push_back
        {
            template <typename Sequence, typename T>
            struct apply : meta::push_back<Sequence, T> {};

            template <typename Sequence, typename T>
            inline typename apply<Sequence const, T>::type
            operator()(Sequence const& seq, T const& x) const
            {
                typedef append_view<Sequence const, T> result;
                return result(seq, x);
            }

            template <typename Sequence, typename T>
            inline typename apply<Sequence, T>::type
            operator()(Sequence& seq, T const& x) const
            {
                typedef append_view<Sequence, T> result;
                return result(seq, x);
            }
        };
    }

    function::push_back const push_back = function::push_back();
}}

#endif

