
#ifndef BOOST_MPL_AUX_CONFIG_DMC_AMBIGUOUS_CTPS_HPP_INCLUDED
#define BOOST_MPL_AUX_CONFIG_DMC_AMBIGUOUS_CTPS_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Id: dmc_ambiguous_ctps.hpp 49267 2008-10-11 06:19:02Z agurtovoy $
// $Date: 2008-10-11 02:19:02 -0400 (Sat, 11 Oct 2008) $
// $Revision: 49267 $

#include <boost/mpl/aux_/config/workaround.hpp>

#if    !defined(BOOST_MPL_CFG_DMC_AMBIGUOUS_CTPS) \
    && !defined(BOOST_MPL_PREPROCESSING_MODE) \
    && BOOST_WORKAROUND(__DMC__, BOOST_TESTED_AT(0x840))

#   define BOOST_MPL_CFG_DMC_AMBIGUOUS_CTPS

#endif

#endif // BOOST_MPL_AUX_CONFIG_DMC_AMBIGUOUS_CTPS_HPP_INCLUDED
