/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_END_HPP)
#define FUSION_SEQUENCE_END_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/sequence/as_fusion_sequence.hpp>
#include <boost/type_traits/is_const.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Tag>
        struct end_impl
        {
            template <typename Sequence>
            struct apply {
                typedef int type;
            };
        };

        template <typename Sequence>
        struct end
        {
            typedef as_fusion_sequence<Sequence> seq_converter;
            typedef typename seq_converter::type seq;

            typedef typename
                end_impl<typename seq::tag>::
                    template apply<seq>::type
            type;
        };
    }

#if BOOST_WORKAROUND(BOOST_MSVC,<=1300)
    namespace detail {
        template <typename Sequence>
        inline typename meta::end<Sequence const>::type
        end(Sequence const& seq,mpl::true_)
        {
            typedef meta::end<Sequence const> end_meta;
            return meta::end_impl<BOOST_DEDUCED_TYPENAME end_meta::seq::tag>::
                template apply<BOOST_DEDUCED_TYPENAME end_meta::seq const>::call(
                    end_meta::seq_converter::convert_const(seq));
        }

        template <typename Sequence>
        inline typename meta::end<Sequence>::type
        end(Sequence& seq,mpl::false_)
        {
            typedef meta::end<Sequence> end_meta;
            return meta::end_impl<BOOST_DEDUCED_TYPENAME end_meta::seq::tag>::
                template apply<BOOST_DEDUCED_TYPENAME end_meta::seq>::call(
                    end_meta::seq_converter::convert(seq));
        }

    }
    template <typename Sequence>
    inline typename meta::end<Sequence>::type
    end(Sequence& seq)
    {
        return detail::end(seq,is_const<Sequence>());
    }
#else
    template <typename Sequence>
    inline typename meta::end<Sequence const>::type
    end(Sequence const& seq)
    {
        typedef meta::end<Sequence const> end_meta;
        return meta::end_impl<typename end_meta::seq::tag>::
            template apply<typename end_meta::seq const>::call(
                end_meta::seq_converter::convert_const(seq));
    }

    template <typename Sequence>
    inline typename meta::end<Sequence>::type
    end(Sequence& seq)
    {
        typedef meta::end<Sequence> end_meta;
        return meta::end_impl<typename end_meta::seq::tag>::
            template apply<typename end_meta::seq>::call(
                end_meta::seq_converter::convert(seq));
    }
#endif
}}

#endif
