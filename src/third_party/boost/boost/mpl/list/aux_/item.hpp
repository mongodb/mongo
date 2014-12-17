
#ifndef BOOST_MPL_LIST_AUX_NODE_HPP_INCLUDED
#define BOOST_MPL_LIST_AUX_NODE_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2000-2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Id: item.hpp 49267 2008-10-11 06:19:02Z agurtovoy $
// $Date: 2008-10-11 02:19:02 -0400 (Sat, 11 Oct 2008) $
// $Revision: 49267 $

#include <boost/mpl/long.hpp>
#include <boost/mpl/list/aux_/tag.hpp>
#include <boost/mpl/aux_/config/msvc.hpp>
#include <boost/mpl/aux_/config/workaround.hpp>

namespace boost { namespace mpl {

template<
      typename Size
    , typename T
    , typename Next
    >
struct l_item
{
// agurt, 17/jul/03: to facilitate the deficient 'is_sequence' implementation 
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
    typedef int begin;
#endif
    typedef aux::list_tag tag;
    typedef l_item type;

    typedef Size size;
    typedef T item;
    typedef Next next;
};

struct l_end
{
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
    typedef int begin;
#endif
    typedef aux::list_tag tag;
    typedef l_end type;
    typedef long_<0> size;
};

}}

#endif // BOOST_MPL_LIST_AUX_NODE_HPP_INCLUDED
