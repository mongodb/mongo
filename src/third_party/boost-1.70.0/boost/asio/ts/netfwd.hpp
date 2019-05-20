//
// ts/netfwd.hpp
// ~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_TS_NETFWD_HPP
#define BOOST_ASIO_TS_NETFWD_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_CHRONO)
# include <boost/asio/detail/chrono.hpp>
#endif // defined(BOOST_ASIO_HAS_CHRONO)

#if defined(BOOST_ASIO_HAS_BOOST_DATE_TIME)
# include <boost/asio/detail/date_time_fwd.hpp>
#endif // defined(BOOST_ASIO_HAS_BOOST_DATE_TIME)

#if !defined(GENERATING_DOCUMENTATION)

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

class execution_context;

template <typename T, typename Executor>
class executor_binder;

template <typename Executor>
class executor_work_guard;

class system_executor;

class executor;

template <typename Executor>
class strand;

class io_context;

template <typename Clock>
struct wait_traits;

#if defined(BOOST_ASIO_HAS_BOOST_DATE_TIME)

template <typename Time>
struct time_traits;

#endif // defined(BOOST_ASIO_HAS_BOOST_DATE_TIME)

#if !defined(BOOST_ASIO_BASIC_WAITABLE_TIMER_FWD_DECL)
#define BOOST_ASIO_BASIC_WAITABLE_TIMER_FWD_DECL

template <typename Clock,
    typename WaitTraits = wait_traits<Clock>,
    typename Executor = executor>
class basic_waitable_timer;

#endif // !defined(BOOST_ASIO_BASIC_WAITABLE_TIMER_FWD_DECL)

#if defined(BOOST_ASIO_HAS_CHRONO)

typedef basic_waitable_timer<chrono::system_clock> system_timer;

typedef basic_waitable_timer<chrono::steady_clock> steady_timer;

typedef basic_waitable_timer<chrono::high_resolution_clock>
  high_resolution_timer;

#endif // defined(BOOST_ASIO_HAS_CHRONO)

#if !defined(BOOST_ASIO_BASIC_SOCKET_FWD_DECL)
#define BOOST_ASIO_BASIC_SOCKET_FWD_DECL

template <typename Protocol, typename Executor = executor>
class basic_socket;

#endif // !defined(BOOST_ASIO_BASIC_SOCKET_FWD_DECL)

#if !defined(BOOST_ASIO_BASIC_DATAGRAM_SOCKET_FWD_DECL)
#define BOOST_ASIO_BASIC_DATAGRAM_SOCKET_FWD_DECL

template <typename Protocol, typename Executor = executor>
class basic_datagram_socket;

#endif // !defined(BOOST_ASIO_BASIC_DATAGRAM_SOCKET_FWD_DECL)

#if !defined(BOOST_ASIO_BASIC_STREAM_SOCKET_FWD_DECL)
#define BOOST_ASIO_BASIC_STREAM_SOCKET_FWD_DECL

// Forward declaration with defaulted arguments.
template <typename Protocol, typename Executor = executor>
class basic_stream_socket;

#endif // !defined(BOOST_ASIO_BASIC_STREAM_SOCKET_FWD_DECL)

#if !defined(BOOST_ASIO_BASIC_SOCKET_ACCEPTOR_FWD_DECL)
#define BOOST_ASIO_BASIC_SOCKET_ACCEPTOR_FWD_DECL

template <typename Protocol, typename Executor = executor>
class basic_socket_acceptor;

#endif // !defined(BOOST_ASIO_BASIC_SOCKET_ACCEPTOR_FWD_DECL)

#if !defined(BOOST_ASIO_BASIC_SOCKET_STREAMBUF_FWD_DECL)
#define BOOST_ASIO_BASIC_SOCKET_STREAMBUF_FWD_DECL

// Forward declaration with defaulted arguments.
template <typename Protocol,
#if defined(BOOST_ASIO_HAS_BOOST_DATE_TIME) \
  || defined(GENERATING_DOCUMENTATION)
    typename Clock = boost::posix_time::ptime,
    typename WaitTraits = time_traits<Clock> >
#else
    typename Clock = chrono::steady_clock,
    typename WaitTraits = wait_traits<Clock> >
#endif
class basic_socket_streambuf;

#endif // !defined(BOOST_ASIO_BASIC_SOCKET_STREAMBUF_FWD_DECL)

#if !defined(BOOST_ASIO_BASIC_SOCKET_IOSTREAM_FWD_DECL)
#define BOOST_ASIO_BASIC_SOCKET_IOSTREAM_FWD_DECL

// Forward declaration with defaulted arguments.
template <typename Protocol,
#if defined(BOOST_ASIO_HAS_BOOST_DATE_TIME) \
  || defined(GENERATING_DOCUMENTATION)
    typename Clock = boost::posix_time::ptime,
    typename WaitTraits = time_traits<Clock> >
#else
    typename Clock = chrono::steady_clock,
    typename WaitTraits = wait_traits<Clock> >
#endif
class basic_socket_iostream;

#endif // !defined(BOOST_ASIO_BASIC_SOCKET_IOSTREAM_FWD_DECL)

namespace ip {

class address;

class address_v4;

class address_v6;

template <typename Address>
class basic_address_iterator;

typedef basic_address_iterator<address_v4> address_v4_iterator;

typedef basic_address_iterator<address_v6> address_v6_iterator;

template <typename Address>
class basic_address_range;

typedef basic_address_range<address_v4> address_v4_range;

typedef basic_address_range<address_v6> address_v6_range;

class network_v4;

class network_v6;

template <typename InternetProtocol>
class basic_endpoint;

template <typename InternetProtocol>
class basic_resolver_entry;

template <typename InternetProtocol>
class basic_resolver_results;

#if !defined(BOOST_ASIO_IP_BASIC_RESOLVER_FWD_DECL)
#define BOOST_ASIO_IP_BASIC_RESOLVER_FWD_DECL

template <typename InternetProtocol, typename Executor = executor>
class basic_resolver;

#endif // !defined(BOOST_ASIO_IP_BASIC_RESOLVER_FWD_DECL)

class tcp;

class udp;

} // namespace ip
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // !defined(GENERATING_DOCUMENTATION)

#endif // BOOST_ASIO_TS_NETFWD_HPP
