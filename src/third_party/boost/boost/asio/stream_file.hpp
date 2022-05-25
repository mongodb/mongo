//
// stream_file.hpp
// ~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_STREAM_FILE_HPP
#define BOOST_ASIO_STREAM_FILE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_FILE) \
  || defined(GENERATING_DOCUMENTATION)

#include <boost/asio/basic_stream_file.hpp>

namespace boost {
namespace asio {

/// Typedef for the typical usage of a stream-oriented file.
typedef basic_stream_file<> stream_file;

} // namespace asio
} // namespace boost

#endif // defined(BOOST_ASIO_HAS_FILE)
       //   || defined(GENERATING_DOCUMENTATION)

#endif // BOOST_ASIO_STREAM_FILE_HPP
