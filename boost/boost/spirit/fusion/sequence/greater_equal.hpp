/*=============================================================================
    Copyright (c) 1999-2003 Jaakko Järvi
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_GREATER_EQUAL_HPP)
#define FUSION_SEQUENCE_GREATER_EQUAL_HPP

#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_greater_equal.hpp>

#ifdef FUSION_COMFORMING_COMPILER
#include <boost/utility/enable_if.hpp>
#include <boost/spirit/fusion/sequence/is_sequence.hpp>
#include <boost/spirit/fusion/sequence/as_fusion_sequence.hpp>
#endif

namespace boost { namespace fusion
{
    template <typename Seq1, typename Seq2>
    inline bool
    operator>=(sequence_base<Seq1> const& a, sequence_base<Seq2> const& b)
    {
        return detail::sequence_greater_equal<Seq1 const, Seq2 const>::
            call(
                fusion::begin(a.cast())
              , fusion::begin(b.cast())
            );
    }

#ifdef FUSION_COMFORMING_COMPILER

    template <typename Seq1, typename Seq2>
    inline typename disable_if<fusion::is_sequence<Seq2>, bool>::type
    operator>=(sequence_base<Seq1> const& a, Seq2 const& b)
    {
        return a >= as_fusion_sequence<Seq2>::convert_const(b);
    }

    template <typename Seq1, typename Seq2>
    inline typename disable_if<fusion::is_sequence<Seq1>, bool>::type
    operator>=(Seq1 const& a, sequence_base<Seq2> const& b)
    {
        return as_fusion_sequence<Seq1>::convert_const(a) >= b;
    }

#endif
}}

#endif
