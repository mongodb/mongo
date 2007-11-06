
#ifndef BOOST_MPL_AUX_CONFIG_COMPILER_HPP_INCLUDED
#define BOOST_MPL_AUX_CONFIG_COMPILER_HPP_INCLUDED

// Copyright Aleksey Gurtovoy 2001-2004
//
// Distributed under the Boost Software License, Version 1.0. 
// (See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/mpl for documentation.

// $Source: /cvsroot/boost/boost/boost/mpl/aux_/config/compiler.hpp,v $
// $Date: 2004/09/02 15:40:45 $
// $Revision: 1.9 $

#if !defined(BOOST_MPL_CFG_COMPILER_DIR)

#   include <boost/mpl/aux_/config/dtp.hpp>
#   include <boost/mpl/aux_/config/ttp.hpp>
#   include <boost/mpl/aux_/config/ctps.hpp>
#   include <boost/mpl/aux_/config/msvc.hpp>
#   include <boost/mpl/aux_/config/gcc.hpp>
#   include <boost/mpl/aux_/config/workaround.hpp>

#   if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
#       define BOOST_MPL_CFG_COMPILER_DIR msvc60

#   elif BOOST_WORKAROUND(BOOST_MSVC, == 1300)
#       define BOOST_MPL_CFG_COMPILER_DIR msvc70

#   elif BOOST_WORKAROUND(BOOST_MPL_CFG_GCC, BOOST_TESTED_AT(0x0304))
#       define BOOST_MPL_CFG_COMPILER_DIR gcc

#   elif BOOST_WORKAROUND(__BORLANDC__, < 0x600)
#       if !defined(BOOST_MPL_CFG_NO_DEFAULT_PARAMETERS_IN_NESTED_TEMPLATES)
#           define BOOST_MPL_CFG_COMPILER_DIR bcc551
#       else
#           define BOOST_MPL_CFG_COMPILER_DIR bcc
#       endif

#   elif BOOST_WORKAROUND(__DMC__, BOOST_TESTED_AT(0x840))
#       define BOOST_MPL_CFG_COMPILER_DIR dmc

#   elif defined(__MWERKS__)
#       if defined(BOOST_MPL_CFG_BROKEN_DEFAULT_PARAMETERS_IN_NESTED_TEMPLATES)
#           define BOOST_MPL_CFG_COMPILER_DIR mwcw
#       else
#           define BOOST_MPL_CFG_COMPILER_DIR plain
#       endif

#   elif defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
#       define BOOST_MPL_CFG_COMPILER_DIR no_ctps

#   elif defined(BOOST_MPL_CFG_NO_TEMPLATE_TEMPLATE_PARAMETERS)
#       define BOOST_MPL_CFG_COMPILER_DIR no_ttp

#   else
#       define BOOST_MPL_CFG_COMPILER_DIR plain
#   endif

#endif // BOOST_MPL_CFG_COMPILER_DIR

#endif // BOOST_MPL_AUX_CONFIG_COMPILER_HPP_INCLUDED
