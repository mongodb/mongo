/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_VALUE_AT_HPP)
#define FUSION_SEQUENCE_VALUE_AT_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/sequence/as_fusion_sequence.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Tag>
        struct value_at_impl
        {
            template <typename Sequence, int N>
            struct apply {
                typedef int type;
            };
        };

        template <typename Sequence, int N>
        struct value_at_c
        {
            typedef as_fusion_sequence<Sequence> seq_converter;
            typedef typename seq_converter::type seq;

            typedef typename
                value_at_impl<FUSION_GET_TAG(seq)>::
                    template apply<seq, N>::type
            type;
        };

        template <typename Sequence, typename N>
        struct value_at : value_at_c<Sequence, N::value> {};
    }
}}

#endif
