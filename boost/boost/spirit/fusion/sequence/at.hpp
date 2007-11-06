/*=============================================================================
    Copyright (c) 1999-2003 Jaakko Järvi
    Copyright (c) 2001-2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_AT_HPP)
#define FUSION_SEQUENCE_AT_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/sequence/as_fusion_sequence.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_base.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Tag>
        struct at_impl
        {
            template <typename Sequence, int N>
            struct apply;
        };

        template <typename Sequence, int N>
        struct at_c
        {
            typedef as_fusion_sequence<Sequence> seq_converter;
            typedef typename seq_converter::type seq;

            typedef typename
                at_impl<FUSION_GET_TAG(seq)>::
                    template apply<seq, N>::type
            type;
        };

        template <typename Sequence, typename N>
        struct at : at_c<Sequence, N::value> {};
    }
#if! BOOST_WORKAROUND(BOOST_MSVC, < 1300)
    template <int N, typename Sequence>
    inline typename meta::at_c<Sequence const, N>::type
    at(sequence_base<Sequence> const& seq FUSION_GET_MSVC_WORKAROUND)
    {
        typedef meta::at_c<Sequence const, N> at_meta;
        return meta::at_impl<typename at_meta::seq::tag>::
            template apply<typename at_meta::seq const, N>::call(
                at_meta::seq_converter::convert_const(seq.cast()));
    }

    template <int N, typename Sequence>
    inline typename meta::at_c<Sequence, N>::type
    at(sequence_base<Sequence>& seq FUSION_GET_MSVC_WORKAROUND)
    {
        typedef meta::at_c<Sequence, N> at_meta;
        return meta::at_impl<typename at_meta::seq::tag>::
            template apply<typename at_meta::seq, N>::call(
                at_meta::seq_converter::convert(seq.cast()));
    }
#endif
}}

#endif

