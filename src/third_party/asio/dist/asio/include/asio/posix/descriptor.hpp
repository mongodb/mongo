//
// posix/descriptor.hpp
// ~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_POSIX_DESCRIPTOR_HPP
#define ASIO_POSIX_DESCRIPTOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"

#if defined(ASIO_HAS_POSIX_STREAM_DESCRIPTOR) \
  || defined(GENERATING_DOCUMENTATION)

#include "asio/posix/basic_descriptor.hpp"

namespace asio {
namespace posix {

/// Typedef for the typical usage of basic_descriptor.
typedef basic_descriptor<> descriptor;

} // namespace posix
} // namespace asio

#endif // defined(ASIO_HAS_POSIX_STREAM_DESCRIPTOR)
       // || defined(GENERATING_DOCUMENTATION)

#endif // ASIO_POSIX_DESCRIPTOR_HPP
