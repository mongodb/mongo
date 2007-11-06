
#ifndef BOOST_MPL_AUX_MSVC_NEVER_TRUE_HPP_INCLUDED
#define BOOST_MPL_AUX_MSVC_NEVER_TRUE_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2000-2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Source: /cvsroot/boost/boost/boost/mpl/aux_/msvc_never_true.hpp,v $
// $Date: 2004/09/02 15:40:43 $
// $Revision: 1.5 $

#include <boost/mpl/aux_/config/msvc.hpp>
#include <boost/mpl/aux_/config/workaround.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)

namespace boost { namespace mpl { namespace aux {

template< typename T >
struct msvc_never_true
{
    enum { value = false };
};

}}}

#endif // BOOST_MSVC

#endif // BOOST_MPL_AUX_MSVC_NEVER_TRUE_HPP_INCLUDED
