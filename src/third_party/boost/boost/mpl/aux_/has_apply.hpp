
#ifndef BOOST_MPL_AUX_HAS_APPLY_HPP_INCLUDED
#define BOOST_MPL_AUX_HAS_APPLY_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Id: has_apply.hpp 49267 2008-10-11 06:19:02Z agurtovoy $
// $Date: 2008-10-11 02:19:02 -0400 (Sat, 11 Oct 2008) $
// $Revision: 49267 $

#include <boost/mpl/has_xxx.hpp>
#include <boost/mpl/aux_/config/has_apply.hpp>

namespace boost { namespace mpl { namespace aux {
#if !defined(BOOST_MPL_CFG_NO_HAS_APPLY)
BOOST_MPL_HAS_XXX_TRAIT_NAMED_DEF(has_apply, apply, false)
#else
template< typename T, typename fallback_ = false_ >
struct has_apply
    : fallback_
{
};
#endif
}}}

#endif // BOOST_MPL_AUX_HAS_APPLY_HPP_INCLUDED
