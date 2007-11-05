
#ifndef BOOST_MPL_DISTANCE_FWD_HPP_INCLUDED
#define BOOST_MPL_DISTANCE_FWD_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2000-2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Source: /cvsroot/boost/boost/boost/mpl/distance_fwd.hpp,v $
// $Date: 2004/11/28 01:53:40 $
// $Revision: 1.3 $

#include <boost/mpl/aux_/common_name_wknd.hpp>

namespace boost { namespace mpl {

BOOST_MPL_AUX_COMMON_NAME_WKND(distance)

template< typename Tag > struct distance_impl;
template< typename First, typename Last > struct distance;

}}

#endif // BOOST_MPL_DISTANCE_FWD_HPP_INCLUDED
