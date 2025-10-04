// Copyright (C) 2013 Vicente J. Botet Escriba
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// 2013/10 Vicente J. Botet Escriba
//   Creation.

#ifndef BOOST_CSBL_MEMORY_ALLOCATOR_ARG_HPP
#define BOOST_CSBL_MEMORY_ALLOCATOR_ARG_HPP

#include <boost/thread/csbl/memory/config.hpp>

// 20.7.6, allocator argument tag

namespace boost
{
  namespace csbl
  {
    using ::std::allocator_arg_t;
    using ::std::allocator_arg;
  }
}

namespace boost
{
  using ::boost::csbl::allocator_arg_t;
  using ::boost::csbl::allocator_arg;
}

#endif // header
