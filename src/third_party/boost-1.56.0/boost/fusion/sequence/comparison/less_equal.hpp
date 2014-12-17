/*=============================================================================
    Copyright (c) 1999-2003 Jaakko Jarvi
    Copyright (c) 2001-2011 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_LESS_EQUAL_05052005_0432)
#define FUSION_LESS_EQUAL_05052005_0432

#include <boost/fusion/support/config.hpp>
#include <boost/fusion/sequence/intrinsic/begin.hpp>
#include <boost/fusion/sequence/intrinsic/end.hpp>
#include <boost/fusion/sequence/intrinsic/size.hpp>
#include <boost/fusion/sequence/comparison/enable_comparison.hpp>
#include <boost/fusion/support/is_sequence.hpp>

#if defined(FUSION_DIRECT_OPERATOR_USAGE)
#include <boost/fusion/sequence/comparison/detail/less_equal.hpp>
#else
#include <boost/fusion/sequence/comparison/less.hpp>
#endif

namespace boost { namespace fusion
{
    template <typename Seq1, typename Seq2>
    BOOST_FUSION_GPU_ENABLED
    inline bool
    less_equal(Seq1 const& a, Seq2 const& b)
    {
#if defined(FUSION_DIRECT_OPERATOR_USAGE)
        return detail::sequence_less_equal<Seq1 const, Seq2 const>::
            call(fusion::begin(a), fusion::begin(b));
#else
        return !(b < a);
#endif
    }

    namespace operators
    {
#if defined(BOOST_MSVC) && (BOOST_MSVC <= 1400)
// Workaround for  VC8.0 and VC7.1
        template <typename Seq1, typename Seq2>
        BOOST_FUSION_GPU_ENABLED
        inline bool
        operator<=(sequence_base<Seq1> const& a, sequence_base<Seq2> const& b)
        {
            return less_equal(a.derived(), b.derived());
        }

        template <typename Seq1, typename Seq2>
        BOOST_FUSION_GPU_ENABLED
        inline typename disable_if<traits::is_native_fusion_sequence<Seq2>, bool>::type
        operator<=(sequence_base<Seq1> const& a, Seq2 const& b)
        {
            return less_equal(a.derived(), b);
        }

        template <typename Seq1, typename Seq2>
        BOOST_FUSION_GPU_ENABLED
        inline typename disable_if<traits::is_native_fusion_sequence<Seq1>, bool>::type
        operator<=(Seq1 const& a, sequence_base<Seq2> const& b)
        {
            return less_equal(a, b.derived());
        }

#else
// Somehow VC8.0 and VC7.1 does not like this code
// but barfs somewhere else.

        template <typename Seq1, typename Seq2>
        BOOST_FUSION_GPU_ENABLED
        inline typename
            boost::enable_if<
                traits::enable_comparison<Seq1, Seq2>
              , bool
            >::type
        operator<=(Seq1 const& a, Seq2 const& b)
        {
            return fusion::less_equal(a, b);
        }
#endif
    }
    using operators::operator<=;
}}

#endif
