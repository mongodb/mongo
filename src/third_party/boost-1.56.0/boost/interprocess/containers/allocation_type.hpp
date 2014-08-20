//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2008-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_CONTAINERS_ALLOCATION_TYPE_HPP
#define BOOST_INTERPROCESS_CONTAINERS_ALLOCATION_TYPE_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/container/detail/allocation_type.hpp>

namespace boost {
namespace interprocess {

/// @cond
typedef int allocation_type;
/// @endcond
static const allocation_type allocate_new       = boost::container::allocate_new;
static const allocation_type expand_fwd         = boost::container::expand_fwd;
static const allocation_type expand_bwd         = boost::container::expand_bwd;
static const allocation_type shrink_in_place    = boost::container::shrink_in_place;
static const allocation_type try_shrink_in_place= boost::container::try_shrink_in_place;
static const allocation_type nothrow_allocation = boost::container::nothrow_allocation;
static const allocation_type zero_memory        = boost::container::zero_memory;

}  //namespace interprocess {
}  //namespace boost {

#include <boost/interprocess/detail/config_end.hpp>

#endif //   #ifndef  BOOST_INTERPROCESS_CONTAINERS_VERSION_TYPE_HPP
