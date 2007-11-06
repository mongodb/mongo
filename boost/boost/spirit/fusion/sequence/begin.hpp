/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_BEGIN_HPP)
#define FUSION_SEQUENCE_BEGIN_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/sequence/as_fusion_sequence.hpp>
#include <boost/type_traits/is_const.hpp>
namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Tag>
        struct begin_impl
        {
            template <typename Sequence>
            struct apply {
                typedef int type;
            };
        };

        template <typename Sequence>
        struct begin
        {
            typedef as_fusion_sequence<Sequence> seq_converter;
            typedef typename seq_converter::type seq;

            typedef typename
                begin_impl<typename seq::tag>::
                    template apply<seq>::type
            type;
        };
    }

#if BOOST_WORKAROUND(BOOST_MSVC,<=1300)
    namespace detail {
        template <typename Sequence>
        inline typename meta::begin<Sequence const>::type
        begin(Sequence const& seq,mpl::bool_<true>)
        {
            typedef meta::begin<Sequence const> begin_meta;
            return meta::begin_impl<BOOST_DEDUCED_TYPENAME begin_meta::seq::tag>::
                template apply<BOOST_DEDUCED_TYPENAME begin_meta::seq const>::call(
                    begin_meta::seq_converter::convert_const(seq));
        }

        template <typename Sequence>
        inline typename meta::begin<Sequence>::type
        begin(Sequence& seq,mpl::bool_<false>)
        {
            typedef meta::begin<Sequence> begin_meta;
            return meta::begin_impl<BOOST_DEDUCED_TYPENAME begin_meta::seq::tag>::
                template apply<BOOST_DEDUCED_TYPENAME begin_meta::seq>::call(
                    begin_meta::seq_converter::convert(seq));
        }
    }

    template <typename Sequence>
    inline typename meta::begin<Sequence>::type
    begin(Sequence& seq)
    {
        return detail::begin(seq,is_const<Sequence>());
    }
#else
    template <typename Sequence>
    inline typename meta::begin<Sequence const>::type
    begin(Sequence const& seq)
    {
        typedef meta::begin<Sequence const> begin_meta;
        return meta::begin_impl<typename begin_meta::seq::tag>::
            template apply<typename begin_meta::seq const>::call(
                begin_meta::seq_converter::convert_const(seq));
    }

    template <typename Sequence>
    inline typename meta::begin<Sequence>::type
    begin(Sequence& seq)
    {
        typedef meta::begin<Sequence> begin_meta;
        return meta::begin_impl<typename begin_meta::seq::tag>::
            template apply<typename begin_meta::seq>::call(
                begin_meta::seq_converter::convert(seq));
    }
#endif
}}

#endif
