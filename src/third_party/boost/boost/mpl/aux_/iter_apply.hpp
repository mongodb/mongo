
#ifndef BOOST_MPL_ITER_APPLY_HPP_INCLUDED
#define BOOST_MPL_ITER_APPLY_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2002-2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Id: iter_apply.hpp 49267 2008-10-11 06:19:02Z agurtovoy $
// $Date: 2008-10-11 02:19:02 -0400 (Sat, 11 Oct 2008) $
// $Revision: 49267 $

#include <boost/mpl/apply.hpp>
#include <boost/mpl/deref.hpp>

namespace boost { namespace mpl { namespace aux {

template<
      typename F
    , typename Iterator
    >
struct iter_apply1
    : apply1< F,typename deref<Iterator>::type >
{
};

template<
      typename F
    , typename Iterator1
    , typename Iterator2
    >
struct iter_apply2
    : apply2<
          F
        , typename deref<Iterator1>::type
        , typename deref<Iterator2>::type
        >
{
};

}}}

#endif // BOOST_MPL_ITER_APPLY_HPP_INCLUDED
