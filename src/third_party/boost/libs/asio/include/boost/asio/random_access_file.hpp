//
// random_access_file.hpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_RANDOM_ACCESS_FILE_HPP
#define BOOST_ASIO_RANDOM_ACCESS_FILE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_FILE) \
  || defined(GENERATING_DOCUMENTATION)

#include <boost/asio/basic_random_access_file.hpp>

namespace boost {
namespace asio {

/// Typedef for the typical usage of a random-access file.
typedef basic_random_access_file<> random_access_file;

} // namespace asio
} // namespace boost

#endif // defined(BOOST_ASIO_HAS_FILE)
       //   || defined(GENERATING_DOCUMENTATION)

#endif // BOOST_ASIO_RANDOM_ACCESS_FILE_HPP
