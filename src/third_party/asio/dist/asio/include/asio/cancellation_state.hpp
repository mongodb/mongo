//
// cancellation_state.hpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_CANCELLATION_STATE_HPP
#define ASIO_CANCELLATION_STATE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <cassert>
#include <new>
#include <utility>
#include "asio/cancellation_signal.hpp"
#include "asio/detail/cstddef.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

/// A simple cancellation signal propagation filter.
template <cancellation_type_t Mask>
struct cancellation_filter
{
  /// Returns <tt>type & Mask</tt>.
  cancellation_type_t operator()(
      cancellation_type_t type) const noexcept
  {
    return type & Mask;
  }
};

/// A cancellation filter that disables cancellation.
typedef cancellation_filter<cancellation_type::none>
  disable_cancellation;

/// A cancellation filter that enables terminal cancellation only.
typedef cancellation_filter<cancellation_type::terminal>
  enable_terminal_cancellation;

#if defined(GENERATING_DOCUMENTATION)

/// A cancellation filter that enables terminal and partial cancellation.
typedef cancellation_filter<
    cancellation_type::terminal | cancellation_type::partial>
  enable_partial_cancellation;

/// A cancellation filter that enables terminal, partial and total cancellation.
typedef cancellation_filter<cancellation_type::terminal
    | cancellation_type::partial | cancellation_type::total>
  enable_total_cancellation;

#else // defined(GENERATING_DOCUMENTATION)

typedef cancellation_filter<
    static_cast<cancellation_type_t>(
      static_cast<unsigned int>(cancellation_type::terminal)
        | static_cast<unsigned int>(cancellation_type::partial))>
  enable_partial_cancellation;

typedef cancellation_filter<
    static_cast<cancellation_type_t>(
      static_cast<unsigned int>(cancellation_type::terminal)
        | static_cast<unsigned int>(cancellation_type::partial)
        | static_cast<unsigned int>(cancellation_type::total))>
  enable_total_cancellation;

#endif // defined(GENERATING_DOCUMENTATION)

/// A cancellation state is used for chaining signals and slots in compositions.
class cancellation_state
{
public:
  /// Construct a disconnected cancellation state.
  constexpr cancellation_state() noexcept
    : impl_(0)
  {
  }

  /// Construct and attach to a parent slot to create a new child slot.
  /**
   * Initialises the cancellation state so that it allows terminal cancellation
   * only. Equivalent to <tt>cancellation_state(slot,
   * enable_terminal_cancellation())</tt>.
   *
   * @param slot The parent cancellation slot to which the state will be
   * attached.
   */
  template <typename CancellationSlot>
  constexpr explicit cancellation_state(CancellationSlot slot)
    : impl_(slot.is_connected() ? &slot.template emplace<impl<>>() : 0)
  {
  }

  /// Construct and attach to a parent slot to create a new child slot.
  /**
   * @param slot The parent cancellation slot to which the state will be
   * attached.
   *
   * @param filter A function object that is used to transform incoming
   * cancellation signals as they are received from the parent slot. This
   * function object must have the signature:
   * @code asio::cancellation_type_t filter(
   *     asio::cancellation_type_t); @endcode
   *
   * The library provides the following pre-defined cancellation filters:
   *
   * @li asio::disable_cancellation
   * @li asio::enable_terminal_cancellation
   * @li asio::enable_partial_cancellation
   * @li asio::enable_total_cancellation
   */
  template <typename CancellationSlot, typename Filter>
  constexpr cancellation_state(CancellationSlot slot, Filter filter)
    : impl_(slot.is_connected()
        ? &slot.template emplace<impl<Filter, Filter>>(filter, filter)
        : 0)
  {
  }

  /// Construct and attach to a parent slot to create a new child slot.
  /**
   * @param slot The parent cancellation slot to which the state will be
   * attached.
   *
   * @param in_filter A function object that is used to transform incoming
   * cancellation signals as they are received from the parent slot. This
   * function object must have the signature:
   * @code asio::cancellation_type_t in_filter(
   *     asio::cancellation_type_t); @endcode
   *
   * @param out_filter A function object that is used to transform outcoming
   * cancellation signals as they are relayed to the child slot. This function
   * object must have the signature:
   * @code asio::cancellation_type_t out_filter(
   *     asio::cancellation_type_t); @endcode
   *
   * The library provides the following pre-defined cancellation filters:
   *
   * @li asio::disable_cancellation
   * @li asio::enable_terminal_cancellation
   * @li asio::enable_partial_cancellation
   * @li asio::enable_total_cancellation
   */
  template <typename CancellationSlot, typename InFilter, typename OutFilter>
  constexpr cancellation_state(CancellationSlot slot,
      InFilter in_filter, OutFilter out_filter)
    : impl_(slot.is_connected()
        ? &slot.template emplace<impl<InFilter, OutFilter>>(
            static_cast<InFilter&&>(in_filter),
            static_cast<OutFilter&&>(out_filter))
        : 0)
  {
  }

  /// Returns the single child slot associated with the state.
  /**
   * This sub-slot is used with the operations that are being composed.
   */
  constexpr cancellation_slot slot() const noexcept
  {
    return impl_ ? impl_->signal_.slot() : cancellation_slot();
  }

  /// Returns the cancellation types that have been triggered.
  cancellation_type_t cancelled() const noexcept
  {
    return impl_ ? impl_->cancelled_ : cancellation_type_t();
  }

  /// Clears the specified cancellation types, if they have been triggered.
  void clear(cancellation_type_t mask = cancellation_type::all)
    noexcept
  {
    if (impl_)
      impl_->cancelled_ &= ~mask;
  }

private:
  struct impl_base
  {
    impl_base()
      : cancelled_()
    {
    }

    cancellation_signal signal_;
    cancellation_type_t cancelled_;
  };

  template <
      typename InFilter = enable_terminal_cancellation,
      typename OutFilter = InFilter>
  struct impl : impl_base
  {
    impl()
      : in_filter_(),
        out_filter_()
    {
    }

    impl(InFilter in_filter, OutFilter out_filter)
      : in_filter_(static_cast<InFilter&&>(in_filter)),
        out_filter_(static_cast<OutFilter&&>(out_filter))
    {
    }

    void operator()(cancellation_type_t in)
    {
      this->cancelled_ = in_filter_(in);
      cancellation_type_t out = out_filter_(this->cancelled_);
      if (out != cancellation_type::none)
        this->signal_.emit(out);
    }

    InFilter in_filter_;
    OutFilter out_filter_;
  };

  impl_base* impl_;
};

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_CANCELLATION_STATE_HPP
