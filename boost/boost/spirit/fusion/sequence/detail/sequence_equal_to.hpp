/*=============================================================================
    Copyright (c) 1999-2003 Jaakko Järvi
    Copyright (c) 2001-2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_SEQUENCE_EQUAL_TO_HPP)
#define FUSION_SEQUENCE_DETAIL_SEQUENCE_EQUAL_TO_HPP

#include <boost/mpl/bool.hpp>
#include <boost/static_assert.hpp>
#include <boost/spirit/fusion/iterator/deref.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>

namespace boost { namespace fusion { namespace detail
{
    namespace sequence_equal_to_detail {
        template<typename T,typename I1,typename I2>
        bool call(T const& self,I1 const& a, I2 const& b) {
            return fusion::deref(a) == fusion::deref(b)
                && T::call(fusion::next(a), fusion::next(b));
        }
    }

    template <typename Seq1, typename Seq2>
    struct sequence_equal_to
    {
        typedef typename meta::end<Seq1>::type end1_type;
        typedef typename meta::end<Seq2>::type end2_type;
        
        template <typename I1, typename I2>
        static bool
        call(I1 const& a, I2 const& b)
        {
            typename meta::equal_to<I1, end1_type>::type eq;
            return call(a, b, eq);
        }

        template <typename I1, typename I2>
        static bool
        call(I1 const&, I2 const&, mpl::true_)
        {
            BOOST_STATIC_ASSERT((meta::equal_to<I2, end2_type>::value));
            return true;
        }

        template <typename I1, typename I2>
        static bool
        call(I1 const& a, I2 const& b, mpl::false_)
        {
            return sequence_equal_to_detail::call(sequence_equal_to<Seq1,Seq2>(),a,b);
        }
    };
}}}

#endif
