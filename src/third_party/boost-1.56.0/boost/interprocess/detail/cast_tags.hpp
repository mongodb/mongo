//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_CAST_TAGS_HPP
#define BOOST_INTERPROCESS_CAST_TAGS_HPP

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>

namespace boost { namespace interprocess { namespace ipcdetail {

struct static_cast_tag {};
struct const_cast_tag {};
struct dynamic_cast_tag {};
struct reinterpret_cast_tag {};

}}}  //namespace boost { namespace interprocess { namespace ipcdetail {

#include <boost/interprocess/detail/config_end.hpp>

#endif   //#ifndef BOOST_INTERPROCESS_CAST_TAGS_HPP

