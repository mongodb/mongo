/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_TYPE_SEQUENCE_HPP)
#define FUSION_SEQUENCE_TYPE_SEQUENCE_HPP

#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/iterator/type_sequence_iterator.hpp>
#include <boost/spirit/fusion/sequence/detail/type_seq_begin_end_trts.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_base.hpp>
#include <boost/type_traits/remove_const.hpp>

namespace boost { namespace fusion
{
    struct type_sequence_tag;

    template <typename Sequence>
    struct type_sequence : sequence_base<type_sequence<Sequence> >
    {
        typedef type_sequence_tag tag;
        typedef typename remove_const<Sequence>::type sequence_type;
    };
}}

#endif


