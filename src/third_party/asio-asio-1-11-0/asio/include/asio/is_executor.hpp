//
// is_executor.hpp
// ~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IS_EXECUTOR_HPP
#define ASIO_IS_EXECUTOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/detail/type_traits.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

/// The is_executor trait detects whether a type T meets the Executor type
/// requirements.
/**
 * Meets the UnaryTypeTrait requirements. The asio library implementation
 * provides a definition that is derived from false_type. A program may
 * specialise this template to derive from true_type for a user-defined type T
 * that meets the Executor requirements.
 */
template <typename T>
struct is_executor : false_type {};

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IS_EXECUTOR_HPP
