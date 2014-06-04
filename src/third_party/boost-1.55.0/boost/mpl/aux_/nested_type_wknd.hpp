
#ifndef BOOST_MPL_AUX_NESTED_TYPE_WKND_HPP_INCLUDED
#define BOOST_MPL_AUX_NESTED_TYPE_WKND_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2000-2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Id: nested_type_wknd.hpp 49267 2008-10-11 06:19:02Z agurtovoy $
// $Date: 2008-10-10 23:19:02 -0700 (Fri, 10 Oct 2008) $
// $Revision: 49267 $

#include <boost/mpl/aux_/config/gcc.hpp>
#include <boost/mpl/aux_/config/workaround.hpp>

#if BOOST_WORKAROUND(BOOST_MPL_CFG_GCC, BOOST_TESTED_AT(0x0302)) \
    || BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x561)) \
    || BOOST_WORKAROUND(__SUNPRO_CC, BOOST_TESTED_AT(0x530)) \
    || BOOST_WORKAROUND(__DMC__, BOOST_TESTED_AT(0x840))

namespace boost { namespace mpl { namespace aux {
template< typename T > struct nested_type_wknd
    : T::type
{
};
}}}

#if BOOST_WORKAROUND(__DMC__, BOOST_TESTED_AT(0x840))
#   define BOOST_MPL_AUX_NESTED_TYPE_WKND(T) \
    aux::nested_type_wknd<T> \
/**/
#else
#   define BOOST_MPL_AUX_NESTED_TYPE_WKND(T) \
    ::boost::mpl::aux::nested_type_wknd<T> \
/**/
#endif

#else // !BOOST_MPL_CFG_GCC et al.

#   define BOOST_MPL_AUX_NESTED_TYPE_WKND(T) T::type

#endif 

#endif // BOOST_MPL_AUX_NESTED_TYPE_WKND_HPP_INCLUDED
