/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_GENERATE_HPP)
#define FUSION_SEQUENCE_GENERATE_HPP

#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>
#include <boost/spirit/fusion/sequence/detail/generate.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_base.hpp>
#include <boost/spirit/fusion/sequence/as_fusion_sequence.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence>
        struct generate
        {
            typedef typename as_fusion_sequence<Sequence>::type seq;
            typedef typename
                detail::result_of_generate<
                    typename meta::begin<seq const>::type
                  , typename meta::end<seq const>::type
                >::type
            type;
        };
    }

    template <typename Sequence>
    inline typename meta::generate<Sequence>::type
    generate(Sequence const& seq)
    {
        typedef as_fusion_sequence<Sequence> converter;
        return detail::generate(
            fusion::begin(converter::convert_const(seq))
          , fusion::end(converter::convert_const(seq)));
    }
}}

#endif


