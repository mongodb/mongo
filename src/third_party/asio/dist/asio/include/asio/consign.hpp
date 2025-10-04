//
// consign.hpp
// ~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_CONSIGN_HPP
#define ASIO_CONSIGN_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <tuple>
#include "asio/detail/type_traits.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

/// Completion token type used to specify that the completion handler should
/// carry additional values along with it.
/**
 * This completion token adapter is typically used to keep at least one copy of
 * an object, such as a smart pointer, alive until the completion handler is
 * called.
 */
template <typename CompletionToken, typename... Values>
class consign_t
{
public:
  /// Constructor.
  template <typename T, typename... V>
  constexpr explicit consign_t(T&& completion_token, V&&... values)
    : token_(static_cast<T&&>(completion_token)),
      values_(static_cast<V&&>(values)...)
  {
  }

#if defined(GENERATING_DOCUMENTATION)
private:
#endif // defined(GENERATING_DOCUMENTATION)
  CompletionToken token_;
  std::tuple<Values...> values_;
};

/// Completion token adapter used to specify that the completion handler should
/// carry additional values along with it.
/**
 * This completion token adapter is typically used to keep at least one copy of
 * an object, such as a smart pointer, alive until the completion handler is
 * called.
 */
template <typename CompletionToken, typename... Values>
ASIO_NODISCARD inline constexpr
consign_t<decay_t<CompletionToken>, decay_t<Values>...>
consign(CompletionToken&& completion_token, Values&&... values)
{
  return consign_t<decay_t<CompletionToken>, decay_t<Values>...>(
      static_cast<CompletionToken&&>(completion_token),
      static_cast<Values&&>(values)...);
}

} // namespace asio

#include "asio/detail/pop_options.hpp"

#include "asio/impl/consign.hpp"

#endif // ASIO_CONSIGN_HPP
