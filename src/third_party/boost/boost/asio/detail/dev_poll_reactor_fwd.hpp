//
// detail/dev_poll_reactor_fwd.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2012 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_DEV_POLL_REACTOR_FWD_HPP
#define BOOST_ASIO_DETAIL_DEV_POLL_REACTOR_FWD_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_DEV_POLL)

namespace boost {
namespace asio {
namespace detail {

class dev_poll_reactor;

} // namespace detail
} // namespace asio
} // namespace boost

#endif // defined(BOOST_ASIO_HAS_DEV_POLL)

#endif // BOOST_ASIO_DETAIL_DEV_POLL_REACTOR_FWD_HPP
