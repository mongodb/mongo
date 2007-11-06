
#ifndef BOOST_MPL_ARG_FWD_HPP_INCLUDED
#define BOOST_MPL_ARG_FWD_HPP_INCLUDED

// Copyright Peter Dimov 2001-2002
// Copyright Aleksey Gurtovoy 2001-2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Source: /cvsroot/boost/boost/boost/mpl/arg_fwd.hpp,v $
// $Date: 2004/09/28 13:56:58 $
// $Revision: 1.6 $

#include <boost/mpl/aux_/adl_barrier.hpp>
#include <boost/mpl/aux_/nttp_decl.hpp>

BOOST_MPL_AUX_ADL_BARRIER_NAMESPACE_OPEN

template< BOOST_MPL_AUX_NTTP_DECL(int, N) > struct arg;

BOOST_MPL_AUX_ADL_BARRIER_NAMESPACE_CLOSE
BOOST_MPL_AUX_ADL_BARRIER_DECL(arg)

#endif // BOOST_MPL_ARG_FWD_HPP_INCLUDED
