
#ifndef BOOST_MPL_AUX_ERASE_IMPL_HPP_INCLUDED
#define BOOST_MPL_AUX_ERASE_IMPL_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2000-2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Source: /cvsroot/boost/boost/boost/mpl/aux_/erase_impl.hpp,v $
// $Date: 2004/09/02 15:40:43 $
// $Revision: 1.4 $

#include <boost/mpl/clear.hpp>
#include <boost/mpl/push_front.hpp>
#include <boost/mpl/reverse_fold.hpp>
#include <boost/mpl/iterator_range.hpp>
#include <boost/mpl/next.hpp>
#include <boost/mpl/aux_/na.hpp>

namespace boost { namespace mpl {

// default implementation; conrete sequences might override it by 
// specializing either the 'erase_impl' or the primary 'erase' template

template< typename Tag >
struct erase_impl
{
    template<
          typename Sequence
        , typename First
        , typename Last
        >
    struct apply
    {
        typedef typename if_na< Last,typename next<First>::type >::type last_;
        
        // 1st half: [begin, first)
        typedef iterator_range<
              typename begin<Sequence>::type
            , First
            > first_half_;

        // 2nd half: [last, end) ... that is, [last + 1, end)
        typedef iterator_range<
              last_
            , typename end<Sequence>::type
            > second_half_;

        typedef typename reverse_fold<
              second_half_
            , typename clear<Sequence>::type
            , push_front<_,_>
            >::type half_sequence_;

        typedef typename reverse_fold<
              first_half_
            , half_sequence_
            , push_front<_,_>
            >::type type;
    };
};

}}

#endif // BOOST_MPL_AUX_ERASE_IMPL_HPP_INCLUDED
