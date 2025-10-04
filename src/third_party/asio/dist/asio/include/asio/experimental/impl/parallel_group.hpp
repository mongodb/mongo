//
// experimental/impl/parallel_group.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IMPL_EXPERIMENTAL_PARALLEL_GROUP_HPP
#define ASIO_IMPL_EXPERIMENTAL_PARALLEL_GROUP_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <new>
#include <tuple>
#include "asio/associated_cancellation_slot.hpp"
#include "asio/detail/recycling_allocator.hpp"
#include "asio/detail/type_traits.hpp"
#include "asio/dispatch.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace experimental {
namespace detail {

// Stores the result from an individual asynchronous operation.
template <typename T, typename = void>
struct parallel_group_op_result
{
public:
  parallel_group_op_result()
    : has_value_(false)
  {
  }

  parallel_group_op_result(parallel_group_op_result&& other)
    : has_value_(other.has_value_)
  {
    if (has_value_)
      new (&u_.value_) T(std::move(other.get()));
  }

  ~parallel_group_op_result()
  {
    if (has_value_)
      u_.value_.~T();
  }

  T& get() noexcept
  {
    return u_.value_;
  }

  template <typename... Args>
  void emplace(Args&&... args)
  {
    new (&u_.value_) T(std::forward<Args>(args)...);
    has_value_ = true;
  }

private:
  union u
  {
    u() {}
    ~u() {}
    char c_;
    T value_;
  } u_;
  bool has_value_;
};

// Proxy completion handler for the group of parallel operations. Unpacks and
// concatenates the individual operations' results, and invokes the user's
// completion handler.
template <typename Handler, typename... Ops>
struct parallel_group_completion_handler
{
  typedef decay_t<
      prefer_result_t<
        associated_executor_t<Handler>,
        execution::outstanding_work_t::tracked_t
      >
    > executor_type;

  parallel_group_completion_handler(Handler&& h)
    : handler_(std::move(h)),
      executor_(
          asio::prefer(
            asio::get_associated_executor(handler_),
            execution::outstanding_work.tracked))
  {
  }

  executor_type get_executor() const noexcept
  {
    return executor_;
  }

  void operator()()
  {
    this->invoke(asio::detail::make_index_sequence<sizeof...(Ops)>());
  }

  template <std::size_t... I>
  void invoke(asio::detail::index_sequence<I...>)
  {
    this->invoke(std::tuple_cat(std::move(std::get<I>(args_).get())...));
  }

  template <typename... Args>
  void invoke(std::tuple<Args...>&& args)
  {
    this->invoke(std::move(args),
        asio::detail::index_sequence_for<Args...>());
  }

  template <typename... Args, std::size_t... I>
  void invoke(std::tuple<Args...>&& args,
      asio::detail::index_sequence<I...>)
  {
    std::move(handler_)(completion_order_, std::move(std::get<I>(args))...);
  }

  Handler handler_;
  executor_type executor_;
  std::array<std::size_t, sizeof...(Ops)> completion_order_{};
  std::tuple<
      parallel_group_op_result<
        typename parallel_op_signature_as_tuple<
          completion_signature_of_t<Ops>
        >::type
      >...
    > args_{};
};

// Shared state for the parallel group.
template <typename Condition, typename Handler, typename... Ops>
struct parallel_group_state
{
  parallel_group_state(Condition&& c, Handler&& h)
    : cancellation_condition_(std::move(c)),
      handler_(std::move(h))
  {
  }

  // The number of operations that have completed so far. Used to determine the
  // order of completion.
  std::atomic<unsigned int> completed_{0};

  // The non-none cancellation type that resulted from a cancellation condition.
  // Stored here for use by the group's initiating function.
  std::atomic<cancellation_type_t> cancel_type_{cancellation_type::none};

  // The number of cancellations that have been requested, either on completion
  // of the operations within the group, or via the cancellation slot for the
  // group operation. Initially set to the number of operations to prevent
  // cancellation signals from being emitted until after all of the group's
  // operations' initiating functions have completed.
  std::atomic<unsigned int> cancellations_requested_{sizeof...(Ops)};

  // The number of operations that are yet to complete. Used to determine when
  // it is safe to invoke the user's completion handler.
  std::atomic<unsigned int> outstanding_{sizeof...(Ops)};

  // The cancellation signals for each operation in the group.
  asio::cancellation_signal cancellation_signals_[sizeof...(Ops)];

  // The cancellation condition is used to determine whether the results from an
  // individual operation warrant a cancellation request for the whole group.
  Condition cancellation_condition_;

  // The proxy handler to be invoked once all operations in the group complete.
  parallel_group_completion_handler<Handler, Ops...> handler_;
};

// Handler for an individual operation within the parallel group.
template <std::size_t I, typename Condition, typename Handler, typename... Ops>
struct parallel_group_op_handler
{
  typedef asio::cancellation_slot cancellation_slot_type;

  parallel_group_op_handler(
    std::shared_ptr<parallel_group_state<Condition, Handler, Ops...>> state)
    : state_(std::move(state))
  {
  }

  cancellation_slot_type get_cancellation_slot() const noexcept
  {
    return state_->cancellation_signals_[I].slot();
  }

  template <typename... Args>
  void operator()(Args... args)
  {
    // Capture this operation into the completion order.
    state_->handler_.completion_order_[state_->completed_++] = I;

    // Determine whether the results of this operation require cancellation of
    // the whole group.
    cancellation_type_t cancel_type = state_->cancellation_condition_(args...);

    // Capture the result of the operation into the proxy completion handler.
    std::get<I>(state_->handler_.args_).emplace(std::move(args)...);

    if (cancel_type != cancellation_type::none)
    {
      // Save the type for potential use by the group's initiating function.
      state_->cancel_type_ = cancel_type;

      // If we are the first operation to request cancellation, emit a signal
      // for each operation in the group.
      if (state_->cancellations_requested_++ == 0)
        for (std::size_t i = 0; i < sizeof...(Ops); ++i)
          if (i != I)
            state_->cancellation_signals_[i].emit(cancel_type);
    }

    // If this is the last outstanding operation, invoke the user's handler.
    if (--state_->outstanding_ == 0)
      asio::dispatch(std::move(state_->handler_));
  }

  std::shared_ptr<parallel_group_state<Condition, Handler, Ops...>> state_;
};

// Handler for an individual operation within the parallel group that has an
// explicitly specified executor.
template <typename Executor, std::size_t I,
    typename Condition, typename Handler, typename... Ops>
struct parallel_group_op_handler_with_executor :
  parallel_group_op_handler<I, Condition, Handler, Ops...>
{
  typedef parallel_group_op_handler<I, Condition, Handler, Ops...> base_type;
  typedef asio::cancellation_slot cancellation_slot_type;
  typedef Executor executor_type;

  parallel_group_op_handler_with_executor(
      std::shared_ptr<parallel_group_state<Condition, Handler, Ops...>> state,
      executor_type ex)
    : parallel_group_op_handler<I, Condition, Handler, Ops...>(std::move(state))
  {
    cancel_proxy_ =
      &this->state_->cancellation_signals_[I].slot().template
        emplace<cancel_proxy>(this->state_, std::move(ex));
  }

  cancellation_slot_type get_cancellation_slot() const noexcept
  {
    return cancel_proxy_->signal_.slot();
  }

  executor_type get_executor() const noexcept
  {
    return cancel_proxy_->executor_;
  }

  // Proxy handler that forwards the emitted signal to the correct executor.
  struct cancel_proxy
  {
    cancel_proxy(
        std::shared_ptr<parallel_group_state<
          Condition, Handler, Ops...>> state,
        executor_type ex)
      : state_(std::move(state)),
        executor_(std::move(ex))
    {
    }

    void operator()(cancellation_type_t type)
    {
      if (auto state = state_.lock())
      {
        asio::cancellation_signal* sig = &signal_;
        asio::dispatch(executor_,
            [state, sig, type]{ sig->emit(type); });
      }
    }

    std::weak_ptr<parallel_group_state<Condition, Handler, Ops...>> state_;
    asio::cancellation_signal signal_;
    executor_type executor_;
  };

  cancel_proxy* cancel_proxy_;
};

// Helper to launch an operation using the correct executor, if any.
template <std::size_t I, typename Op, typename = void>
struct parallel_group_op_launcher
{
  template <typename Condition, typename Handler, typename... Ops>
  static void launch(Op& op,
    const std::shared_ptr<parallel_group_state<
      Condition, Handler, Ops...>>& state)
  {
    typedef associated_executor_t<Op> ex_type;
    ex_type ex = asio::get_associated_executor(op);
    std::move(op)(
        parallel_group_op_handler_with_executor<ex_type, I,
          Condition, Handler, Ops...>(state, std::move(ex)));
  }
};

// Specialised launcher for operations that specify no executor.
template <std::size_t I, typename Op>
struct parallel_group_op_launcher<I, Op,
    enable_if_t<
      is_same<
        typename associated_executor<
          Op>::asio_associated_executor_is_unspecialised,
        void
      >::value
    >>
{
  template <typename Condition, typename Handler, typename... Ops>
  static void launch(Op& op,
    const std::shared_ptr<parallel_group_state<
      Condition, Handler, Ops...>>& state)
  {
    std::move(op)(
        parallel_group_op_handler<I, Condition, Handler, Ops...>(state));
  }
};

template <typename Condition, typename Handler, typename... Ops>
struct parallel_group_cancellation_handler
{
  parallel_group_cancellation_handler(
    std::shared_ptr<parallel_group_state<Condition, Handler, Ops...>> state)
    : state_(std::move(state))
  {
  }

  void operator()(cancellation_type_t cancel_type)
  {
    // If we are the first place to request cancellation, i.e. no operation has
    // yet completed and requested cancellation, emit a signal for each
    // operation in the group.
    if (cancel_type != cancellation_type::none)
      if (auto state = state_.lock())
        if (state->cancellations_requested_++ == 0)
          for (std::size_t i = 0; i < sizeof...(Ops); ++i)
            state->cancellation_signals_[i].emit(cancel_type);
  }

  std::weak_ptr<parallel_group_state<Condition, Handler, Ops...>> state_;
};

template <typename Condition, typename Handler,
    typename... Ops, std::size_t... I>
void parallel_group_launch(Condition cancellation_condition, Handler handler,
    std::tuple<Ops...>& ops, asio::detail::index_sequence<I...>)
{
  // Get the user's completion handler's cancellation slot, so that we can allow
  // cancellation of the entire group.
  associated_cancellation_slot_t<Handler> slot
    = asio::get_associated_cancellation_slot(handler);

  // Create the shared state for the operation.
  typedef parallel_group_state<Condition, Handler, Ops...> state_type;
  std::shared_ptr<state_type> state = std::allocate_shared<state_type>(
      asio::detail::recycling_allocator<state_type,
        asio::detail::thread_info_base::parallel_group_tag>(),
      std::move(cancellation_condition), std::move(handler));

  // Initiate each individual operation in the group.
  int fold[] = { 0,
    ( parallel_group_op_launcher<I, Ops>::launch(std::get<I>(ops), state),
      0 )...
  };
  (void)fold;

  // Check if any of the operations has already requested cancellation, and if
  // so, emit a signal for each operation in the group.
  if ((state->cancellations_requested_ -= sizeof...(Ops)) > 0)
    for (auto& signal : state->cancellation_signals_)
      signal.emit(state->cancel_type_);

  // Register a handler with the user's completion handler's cancellation slot.
  if (slot.is_connected())
    slot.template emplace<
      parallel_group_cancellation_handler<
        Condition, Handler, Ops...>>(state);
}

// Proxy completion handler for the ranged group of parallel operations.
// Unpacks and recombines the individual operations' results, and invokes the
// user's completion handler.
template <typename Handler, typename Op, typename Allocator>
struct ranged_parallel_group_completion_handler
{
  typedef decay_t<
      prefer_result_t<
        associated_executor_t<Handler>,
        execution::outstanding_work_t::tracked_t
      >
    > executor_type;

  typedef typename parallel_op_signature_as_tuple<
      completion_signature_of_t<Op>
    >::type op_tuple_type;

  typedef parallel_group_op_result<op_tuple_type> op_result_type;

  ranged_parallel_group_completion_handler(Handler&& h,
      std::size_t size, const Allocator& allocator)
    : handler_(std::move(h)),
      executor_(
          asio::prefer(
            asio::get_associated_executor(handler_),
            execution::outstanding_work.tracked)),
      allocator_(allocator),
      completion_order_(size, 0,
          ASIO_REBIND_ALLOC(Allocator, std::size_t)(allocator)),
      args_(ASIO_REBIND_ALLOC(Allocator, op_result_type)(allocator))
  {
    for (std::size_t i = 0; i < size; ++i)
      args_.emplace_back();
  }

  executor_type get_executor() const noexcept
  {
    return executor_;
  }

  void operator()()
  {
    this->invoke(
        asio::detail::make_index_sequence<
          std::tuple_size<op_tuple_type>::value>());
  }

  template <std::size_t... I>
  void invoke(asio::detail::index_sequence<I...>)
  {
    typedef typename parallel_op_signature_as_tuple<
        typename ranged_parallel_group_signature<
          completion_signature_of_t<Op>,
          Allocator
        >::raw_type
      >::type vectors_type;

    // Construct all result vectors using the supplied allocator.
    vectors_type vectors{
        typename std::tuple_element<I, vectors_type>::type(
          ASIO_REBIND_ALLOC(Allocator, int)(allocator_))...};

    // Reserve sufficient space in each of the result vectors.
    int reserve_fold[] = { 0,
      ( std::get<I>(vectors).reserve(completion_order_.size()),
        0 )...
    };
    (void)reserve_fold;

    // Copy the results from all operations into the result vectors.
    for (std::size_t idx = 0; idx < completion_order_.size(); ++idx)
    {
      int pushback_fold[] = { 0,
        ( std::get<I>(vectors).push_back(
            std::move(std::get<I>(args_[idx].get()))),
          0 )...
      };
      (void)pushback_fold;
    }

    std::move(handler_)(std::move(completion_order_),
        std::move(std::get<I>(vectors))...);
  }

  Handler handler_;
  executor_type executor_;
  Allocator allocator_;
  std::vector<std::size_t,
    ASIO_REBIND_ALLOC(Allocator, std::size_t)> completion_order_;
  std::deque<op_result_type,
    ASIO_REBIND_ALLOC(Allocator, op_result_type)> args_;
};

// Shared state for the parallel group.
template <typename Condition, typename Handler, typename Op, typename Allocator>
struct ranged_parallel_group_state
{
  ranged_parallel_group_state(Condition&& c, Handler&& h,
      std::size_t size, const Allocator& allocator)
    : cancellations_requested_(size),
      outstanding_(size),
      cancellation_signals_(
          ASIO_REBIND_ALLOC(Allocator,
            asio::cancellation_signal)(allocator)),
      cancellation_condition_(std::move(c)),
      handler_(std::move(h), size, allocator)
  {
    for (std::size_t i = 0; i < size; ++i)
      cancellation_signals_.emplace_back();
  }

  // The number of operations that have completed so far. Used to determine the
  // order of completion.
  std::atomic<unsigned int> completed_{0};

  // The non-none cancellation type that resulted from a cancellation condition.
  // Stored here for use by the group's initiating function.
  std::atomic<cancellation_type_t> cancel_type_{cancellation_type::none};

  // The number of cancellations that have been requested, either on completion
  // of the operations within the group, or via the cancellation slot for the
  // group operation. Initially set to the number of operations to prevent
  // cancellation signals from being emitted until after all of the group's
  // operations' initiating functions have completed.
  std::atomic<unsigned int> cancellations_requested_;

  // The number of operations that are yet to complete. Used to determine when
  // it is safe to invoke the user's completion handler.
  std::atomic<unsigned int> outstanding_;

  // The cancellation signals for each operation in the group.
  std::deque<asio::cancellation_signal,
    ASIO_REBIND_ALLOC(Allocator, asio::cancellation_signal)>
      cancellation_signals_;

  // The cancellation condition is used to determine whether the results from an
  // individual operation warrant a cancellation request for the whole group.
  Condition cancellation_condition_;

  // The proxy handler to be invoked once all operations in the group complete.
  ranged_parallel_group_completion_handler<Handler, Op, Allocator> handler_;
};

// Handler for an individual operation within the parallel group.
template <typename Condition, typename Handler, typename Op, typename Allocator>
struct ranged_parallel_group_op_handler
{
  typedef asio::cancellation_slot cancellation_slot_type;

  ranged_parallel_group_op_handler(
      std::shared_ptr<ranged_parallel_group_state<
        Condition, Handler, Op, Allocator>> state,
      std::size_t idx)
    : state_(std::move(state)),
      idx_(idx)
  {
  }

  cancellation_slot_type get_cancellation_slot() const noexcept
  {
    return state_->cancellation_signals_[idx_].slot();
  }

  template <typename... Args>
  void operator()(Args... args)
  {
    // Capture this operation into the completion order.
    state_->handler_.completion_order_[state_->completed_++] = idx_;

    // Determine whether the results of this operation require cancellation of
    // the whole group.
    cancellation_type_t cancel_type = state_->cancellation_condition_(args...);

    // Capture the result of the operation into the proxy completion handler.
    state_->handler_.args_[idx_].emplace(std::move(args)...);

    if (cancel_type != cancellation_type::none)
    {
      // Save the type for potential use by the group's initiating function.
      state_->cancel_type_ = cancel_type;

      // If we are the first operation to request cancellation, emit a signal
      // for each operation in the group.
      if (state_->cancellations_requested_++ == 0)
        for (std::size_t i = 0; i < state_->cancellation_signals_.size(); ++i)
          if (i != idx_)
            state_->cancellation_signals_[i].emit(cancel_type);
    }

    // If this is the last outstanding operation, invoke the user's handler.
    if (--state_->outstanding_ == 0)
      asio::dispatch(std::move(state_->handler_));
  }

  std::shared_ptr<ranged_parallel_group_state<
    Condition, Handler, Op, Allocator>> state_;
  std::size_t idx_;
};

// Handler for an individual operation within the parallel group that has an
// explicitly specified executor.
template <typename Executor, typename Condition,
    typename Handler, typename Op, typename Allocator>
struct ranged_parallel_group_op_handler_with_executor :
  ranged_parallel_group_op_handler<Condition, Handler, Op, Allocator>
{
  typedef ranged_parallel_group_op_handler<
    Condition, Handler, Op, Allocator> base_type;
  typedef asio::cancellation_slot cancellation_slot_type;
  typedef Executor executor_type;

  ranged_parallel_group_op_handler_with_executor(
      std::shared_ptr<ranged_parallel_group_state<
        Condition, Handler, Op, Allocator>> state,
      executor_type ex, std::size_t idx)
    : ranged_parallel_group_op_handler<Condition, Handler, Op, Allocator>(
        std::move(state), idx)
  {
    cancel_proxy_ =
      &this->state_->cancellation_signals_[idx].slot().template
        emplace<cancel_proxy>(this->state_, std::move(ex));
  }

  cancellation_slot_type get_cancellation_slot() const noexcept
  {
    return cancel_proxy_->signal_.slot();
  }

  executor_type get_executor() const noexcept
  {
    return cancel_proxy_->executor_;
  }

  // Proxy handler that forwards the emitted signal to the correct executor.
  struct cancel_proxy
  {
    cancel_proxy(
        std::shared_ptr<ranged_parallel_group_state<
          Condition, Handler, Op, Allocator>> state,
        executor_type ex)
      : state_(std::move(state)),
        executor_(std::move(ex))
    {
    }

    void operator()(cancellation_type_t type)
    {
      if (auto state = state_.lock())
      {
        asio::cancellation_signal* sig = &signal_;
        asio::dispatch(executor_,
            [state, sig, type]{ sig->emit(type); });
      }
    }

    std::weak_ptr<ranged_parallel_group_state<
      Condition, Handler, Op, Allocator>> state_;
    asio::cancellation_signal signal_;
    executor_type executor_;
  };

  cancel_proxy* cancel_proxy_;
};

template <typename Condition, typename Handler, typename Op, typename Allocator>
struct ranged_parallel_group_cancellation_handler
{
  ranged_parallel_group_cancellation_handler(
      std::shared_ptr<ranged_parallel_group_state<
        Condition, Handler, Op, Allocator>> state)
    : state_(std::move(state))
  {
  }

  void operator()(cancellation_type_t cancel_type)
  {
    // If we are the first place to request cancellation, i.e. no operation has
    // yet completed and requested cancellation, emit a signal for each
    // operation in the group.
    if (cancel_type != cancellation_type::none)
      if (auto state = state_.lock())
        if (state->cancellations_requested_++ == 0)
          for (std::size_t i = 0; i < state->cancellation_signals_.size(); ++i)
            state->cancellation_signals_[i].emit(cancel_type);
  }

  std::weak_ptr<ranged_parallel_group_state<
    Condition, Handler, Op, Allocator>> state_;
};

template <typename Condition, typename Handler,
    typename Range, typename Allocator>
void ranged_parallel_group_launch(Condition cancellation_condition,
    Handler handler, Range&& range, const Allocator& allocator)
{
  // Get the user's completion handler's cancellation slot, so that we can allow
  // cancellation of the entire group.
  associated_cancellation_slot_t<Handler> slot
    = asio::get_associated_cancellation_slot(handler);

  // The type of the asynchronous operation.
  typedef decay_t<decltype(*declval<typename Range::iterator>())> op_type;

  // Create the shared state for the operation.
  typedef ranged_parallel_group_state<Condition,
    Handler, op_type, Allocator> state_type;
  std::shared_ptr<state_type> state = std::allocate_shared<state_type>(
      asio::detail::recycling_allocator<state_type,
        asio::detail::thread_info_base::parallel_group_tag>(),
      std::move(cancellation_condition),
      std::move(handler), range.size(), allocator);

  std::size_t idx = 0;
  std::size_t range_size = range.size();
  for (auto&& op : std::forward<Range>(range))
  {
    typedef associated_executor_t<op_type> ex_type;
    ex_type ex = asio::get_associated_executor(op);
    std::move(op)(
        ranged_parallel_group_op_handler_with_executor<
          ex_type, Condition, Handler, op_type, Allocator>(
            state, std::move(ex), idx++));
  }

  // Check if any of the operations has already requested cancellation, and if
  // so, emit a signal for each operation in the group.
  if ((state->cancellations_requested_ -= range_size) > 0)
    for (auto& signal : state->cancellation_signals_)
      signal.emit(state->cancel_type_);

  // Register a handler with the user's completion handler's cancellation slot.
  if (slot.is_connected())
    slot.template emplace<
      ranged_parallel_group_cancellation_handler<
        Condition, Handler, op_type, Allocator>>(state);
}

} // namespace detail
} // namespace experimental

template <template <typename, typename> class Associator,
    typename Handler, typename... Ops, typename DefaultCandidate>
struct associator<Associator,
    experimental::detail::parallel_group_completion_handler<Handler, Ops...>,
    DefaultCandidate>
  : Associator<Handler, DefaultCandidate>
{
  static typename Associator<Handler, DefaultCandidate>::type get(
      const experimental::detail::parallel_group_completion_handler<
        Handler, Ops...>& h) noexcept
  {
    return Associator<Handler, DefaultCandidate>::get(h.handler_);
  }

  static auto get(
      const experimental::detail::parallel_group_completion_handler<
        Handler, Ops...>& h,
      const DefaultCandidate& c) noexcept
    -> decltype(Associator<Handler, DefaultCandidate>::get(h.handler_, c))
  {
    return Associator<Handler, DefaultCandidate>::get(h.handler_, c);
  }
};

template <template <typename, typename> class Associator, typename Handler,
    typename Op, typename Allocator, typename DefaultCandidate>
struct associator<Associator,
    experimental::detail::ranged_parallel_group_completion_handler<
      Handler, Op, Allocator>,
    DefaultCandidate>
  : Associator<Handler, DefaultCandidate>
{
  static typename Associator<Handler, DefaultCandidate>::type get(
      const experimental::detail::ranged_parallel_group_completion_handler<
        Handler, Op, Allocator>& h) noexcept
  {
    return Associator<Handler, DefaultCandidate>::get(h.handler_);
  }

  static auto get(
      const experimental::detail::ranged_parallel_group_completion_handler<
        Handler, Op, Allocator>& h,
      const DefaultCandidate& c) noexcept
    -> decltype(Associator<Handler, DefaultCandidate>::get(h.handler_, c))
  {
    return Associator<Handler, DefaultCandidate>::get(h.handler_, c);
  }
};

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IMPL_EXPERIMENTAL_PARALLEL_GROUP_HPP
