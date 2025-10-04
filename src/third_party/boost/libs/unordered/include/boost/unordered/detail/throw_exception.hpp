// Copyright (C) 2023 Braden Ganetsky
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_THROW_EXCEPTION_HPP
#define BOOST_UNORDERED_DETAIL_THROW_EXCEPTION_HPP

#include <boost/config.hpp>
#if defined(BOOST_HAS_PRAGMA_ONCE)
#pragma once
#endif

#include <boost/throw_exception.hpp>
#include <stdexcept>

namespace boost {
  namespace unordered {
    namespace detail {

      BOOST_NOINLINE BOOST_NORETURN inline void throw_out_of_range(
        char const* message)
      {
        boost::throw_exception(std::out_of_range(message));
      }

    } // namespace detail
  } // namespace unordered
} // namespace boost

#endif // BOOST_UNORDERED_DETAIL_THROW_EXCEPTION_HPP
