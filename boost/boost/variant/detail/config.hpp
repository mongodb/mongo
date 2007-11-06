//-----------------------------------------------------------------------------
// boost variant/detail/config.hpp header file
// See http://www.boost.org for updates, documentation, and revision history.
//-----------------------------------------------------------------------------
//
// Copyright (c) 2003
// Eric Friedman
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_VARIANT_DETAIL_CONFIG_HPP
#define BOOST_VARIANT_DETAIL_CONFIG_HPP

#include "boost/config.hpp"
#include "boost/detail/workaround.hpp"

///////////////////////////////////////////////////////////////////////////////
// macro BOOST_VARIANT_AUX_BROKEN_CONSTRUCTOR_TEMPLATE_ORDERING
//
#if BOOST_WORKAROUND(__MWERKS__, <= 0x3201) \
 || BOOST_WORKAROUND(BOOST_INTEL, <= 700) \
 || BOOST_WORKAROUND(BOOST_MSVC, <= 1300) \
 && !defined(BOOST_VARIANT_AUX_BROKEN_CONSTRUCTOR_TEMPLATE_ORDERING)
#   define BOOST_VARIANT_AUX_BROKEN_CONSTRUCTOR_TEMPLATE_ORDERING
#endif

///////////////////////////////////////////////////////////////////////////////
// macro BOOST_VARIANT_AUX_HAS_CONSTRUCTOR_TEMPLATE_ORDERING_SFINAE_WKND
//
#if !defined(BOOST_NO_SFINAE) \
 && !BOOST_WORKAROUND(BOOST_INTEL, <= 700) \
 && !defined(BOOST_VARIANT_AUX_HAS_CONSTRUCTOR_TEMPLATE_ORDERING_SFINAE_WKND)
#   define BOOST_VARIANT_AUX_HAS_CONSTRUCTOR_TEMPLATE_ORDERING_SFINAE_WKND
#endif

#endif // BOOST_VARIANT_DETAIL_CONFIG_HPP
