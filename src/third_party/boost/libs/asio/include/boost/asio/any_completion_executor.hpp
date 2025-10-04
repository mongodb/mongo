//
// any_completion_executor.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_ANY_COMPLETION_EXECUTOR_HPP
#define BOOST_ASIO_ANY_COMPLETION_EXECUTOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#if defined(BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT)
# include <boost/asio/executor.hpp>
#else // defined(BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT)
# include <boost/asio/execution.hpp>
#endif // defined(BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT)

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

#if defined(BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT)

typedef executor any_completion_executor;

#else // defined(BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT)

/// Polymorphic executor type for use with I/O objects.
/**
 * The @c any_completion_executor type is a polymorphic executor that supports
 * the set of properties required for the execution of completion handlers. It
 * is defined as the execution::any_executor class template parameterised as
 * follows:
 * @code execution::any_executor<
 *   execution::prefer_only<execution::outstanding_work_t::tracked_t>,
 *   execution::prefer_only<execution::outstanding_work_t::untracked_t>
 *   execution::prefer_only<execution::relationship_t::fork_t>,
 *   execution::prefer_only<execution::relationship_t::continuation_t>
 * > @endcode
 */
class any_completion_executor :
#if defined(GENERATING_DOCUMENTATION)
  public execution::any_executor<...>
#else // defined(GENERATING_DOCUMENTATION)
  public execution::any_executor<
      execution::prefer_only<execution::outstanding_work_t::tracked_t>,
      execution::prefer_only<execution::outstanding_work_t::untracked_t>,
      execution::prefer_only<execution::relationship_t::fork_t>,
      execution::prefer_only<execution::relationship_t::continuation_t>
    >
#endif // defined(GENERATING_DOCUMENTATION)
{
public:
#if !defined(GENERATING_DOCUMENTATION)
  typedef execution::any_executor<
      execution::prefer_only<execution::outstanding_work_t::tracked_t>,
      execution::prefer_only<execution::outstanding_work_t::untracked_t>,
      execution::prefer_only<execution::relationship_t::fork_t>,
      execution::prefer_only<execution::relationship_t::continuation_t>
    > base_type;

  typedef void supportable_properties_type(
      execution::prefer_only<execution::outstanding_work_t::tracked_t>,
      execution::prefer_only<execution::outstanding_work_t::untracked_t>,
      execution::prefer_only<execution::relationship_t::fork_t>,
      execution::prefer_only<execution::relationship_t::continuation_t>
    );
#endif // !defined(GENERATING_DOCUMENTATION)

  /// Default constructor.
  BOOST_ASIO_DECL any_completion_executor() noexcept;

  /// Construct in an empty state. Equivalent effects to default constructor.
  BOOST_ASIO_DECL any_completion_executor(nullptr_t) noexcept;

  /// Copy constructor.
  BOOST_ASIO_DECL any_completion_executor(
      const any_completion_executor& e) noexcept;

  /// Move constructor.
  BOOST_ASIO_DECL any_completion_executor(
      any_completion_executor&& e) noexcept;

  /// Construct to point to the same target as another any_executor.
#if defined(GENERATING_DOCUMENTATION)
  template <class... OtherSupportableProperties>
    any_completion_executor(
        execution::any_executor<OtherSupportableProperties...> e);
#else // defined(GENERATING_DOCUMENTATION)
  template <typename OtherAnyExecutor>
  any_completion_executor(OtherAnyExecutor e,
      constraint_t<
        conditional<
          !is_same<OtherAnyExecutor, any_completion_executor>::value
            && is_base_of<execution::detail::any_executor_base,
              OtherAnyExecutor>::value,
          typename execution::detail::supportable_properties<
            0, supportable_properties_type>::template
              is_valid_target<OtherAnyExecutor>,
          false_type
        >::type::value
      > = 0)
    : base_type(static_cast<OtherAnyExecutor&&>(e))
  {
  }
#endif // defined(GENERATING_DOCUMENTATION)

  /// Construct to point to the same target as another any_executor.
#if defined(GENERATING_DOCUMENTATION)
  template <class... OtherSupportableProperties>
    any_completion_executor(std::nothrow_t,
      execution::any_executor<OtherSupportableProperties...> e);
#else // defined(GENERATING_DOCUMENTATION)
  template <typename OtherAnyExecutor>
  any_completion_executor(std::nothrow_t, OtherAnyExecutor e,
      constraint_t<
        conditional<
          !is_same<OtherAnyExecutor, any_completion_executor>::value
            && is_base_of<execution::detail::any_executor_base,
              OtherAnyExecutor>::value,
          typename execution::detail::supportable_properties<
            0, supportable_properties_type>::template
              is_valid_target<OtherAnyExecutor>,
          false_type
        >::type::value
      > = 0) noexcept
    : base_type(std::nothrow, static_cast<OtherAnyExecutor&&>(e))
  {
  }
#endif // defined(GENERATING_DOCUMENTATION)

  /// Construct to point to the same target as another any_executor.
  BOOST_ASIO_DECL any_completion_executor(std::nothrow_t,
      const any_completion_executor& e) noexcept;

  /// Construct to point to the same target as another any_executor.
  BOOST_ASIO_DECL any_completion_executor(std::nothrow_t,
      any_completion_executor&& e) noexcept;

  /// Construct a polymorphic wrapper for the specified executor.
#if defined(GENERATING_DOCUMENTATION)
  template <BOOST_ASIO_EXECUTION_EXECUTOR Executor>
  any_completion_executor(Executor e);
#else // defined(GENERATING_DOCUMENTATION)
  template <BOOST_ASIO_EXECUTION_EXECUTOR Executor>
  any_completion_executor(Executor e,
      constraint_t<
        conditional<
          !is_same<Executor, any_completion_executor>::value
            && !is_base_of<execution::detail::any_executor_base,
              Executor>::value,
          execution::detail::is_valid_target_executor<
            Executor, supportable_properties_type>,
          false_type
        >::type::value
      > = 0)
    : base_type(static_cast<Executor&&>(e))
  {
  }
#endif // defined(GENERATING_DOCUMENTATION)

  /// Construct a polymorphic wrapper for the specified executor.
#if defined(GENERATING_DOCUMENTATION)
  template <BOOST_ASIO_EXECUTION_EXECUTOR Executor>
  any_completion_executor(std::nothrow_t, Executor e);
#else // defined(GENERATING_DOCUMENTATION)
  template <BOOST_ASIO_EXECUTION_EXECUTOR Executor>
  any_completion_executor(std::nothrow_t, Executor e,
      constraint_t<
        conditional<
          !is_same<Executor, any_completion_executor>::value
            && !is_base_of<execution::detail::any_executor_base,
              Executor>::value,
          execution::detail::is_valid_target_executor<
            Executor, supportable_properties_type>,
          false_type
        >::type::value
      > = 0) noexcept
    : base_type(std::nothrow, static_cast<Executor&&>(e))
  {
  }
#endif // defined(GENERATING_DOCUMENTATION)

  /// Assignment operator.
  BOOST_ASIO_DECL any_completion_executor& operator=(
      const any_completion_executor& e) noexcept;

  /// Move assignment operator.
  BOOST_ASIO_DECL any_completion_executor& operator=(
      any_completion_executor&& e) noexcept;

  /// Assignment operator that sets the polymorphic wrapper to the empty state.
  BOOST_ASIO_DECL any_completion_executor& operator=(nullptr_t);

  /// Destructor.
  BOOST_ASIO_DECL ~any_completion_executor();

  /// Swap targets with another polymorphic wrapper.
  BOOST_ASIO_DECL void swap(any_completion_executor& other) noexcept;

  /// Obtain a polymorphic wrapper with the specified property.
  /**
   * Do not call this function directly. It is intended for use with the
   * boost::asio::require and boost::asio::prefer customisation points.
   *
   * For example:
   * @code any_completion_executor ex = ...;
   * auto ex2 = boost::asio::require(ex, execution::relationship.fork); @endcode
   */
  template <typename Property>
  any_completion_executor require(const Property& p,
      constraint_t<
        traits::require_member<const base_type&, const Property&>::is_valid
      > = 0) const
  {
    return static_cast<const base_type&>(*this).require(p);
  }

  /// Obtain a polymorphic wrapper with the specified property.
  /**
   * Do not call this function directly. It is intended for use with the
   * boost::asio::prefer customisation point.
   *
   * For example:
   * @code any_completion_executor ex = ...;
   * auto ex2 = boost::asio::prefer(ex, execution::relationship.fork); @endcode
   */
  template <typename Property>
  any_completion_executor prefer(const Property& p,
      constraint_t<
        traits::prefer_member<const base_type&, const Property&>::is_valid
      > = 0) const
  {
    return static_cast<const base_type&>(*this).prefer(p);
  }
};

#if !defined(GENERATING_DOCUMENTATION)

template <>
BOOST_ASIO_DECL any_completion_executor any_completion_executor::prefer(
    const execution::outstanding_work_t::tracked_t&, int) const;

template <>
BOOST_ASIO_DECL any_completion_executor any_completion_executor::prefer(
    const execution::outstanding_work_t::untracked_t&, int) const;

template <>
BOOST_ASIO_DECL any_completion_executor any_completion_executor::prefer(
    const execution::relationship_t::fork_t&, int) const;

template <>
BOOST_ASIO_DECL any_completion_executor any_completion_executor::prefer(
    const execution::relationship_t::continuation_t&, int) const;

namespace traits {

#if !defined(BOOST_ASIO_HAS_DEDUCED_EQUALITY_COMPARABLE_TRAIT)

template <>
struct equality_comparable<any_completion_executor>
{
  static const bool is_valid = true;
  static const bool is_noexcept = true;
};

#endif // !defined(BOOST_ASIO_HAS_DEDUCED_EQUALITY_COMPARABLE_TRAIT)

#if !defined(BOOST_ASIO_HAS_DEDUCED_EXECUTE_MEMBER_TRAIT)

template <typename F>
struct execute_member<any_completion_executor, F>
{
  static const bool is_valid = true;
  static const bool is_noexcept = false;
  typedef void result_type;
};

#endif // !defined(BOOST_ASIO_HAS_DEDUCED_EXECUTE_MEMBER_TRAIT)

#if !defined(BOOST_ASIO_HAS_DEDUCED_QUERY_MEMBER_TRAIT)

template <typename Prop>
struct query_member<any_completion_executor, Prop> :
  query_member<any_completion_executor::base_type, Prop>
{
};

#endif // !defined(BOOST_ASIO_HAS_DEDUCED_QUERY_MEMBER_TRAIT)

#if !defined(BOOST_ASIO_HAS_DEDUCED_REQUIRE_MEMBER_TRAIT)

template <typename Prop>
struct require_member<any_completion_executor, Prop> :
  require_member<any_completion_executor::base_type, Prop>
{
  typedef any_completion_executor result_type;
};

#endif // !defined(BOOST_ASIO_HAS_DEDUCED_REQUIRE_MEMBER_TRAIT)

#if !defined(BOOST_ASIO_HAS_DEDUCED_PREFER_MEMBER_TRAIT)

template <typename Prop>
struct prefer_member<any_completion_executor, Prop> :
  prefer_member<any_completion_executor::base_type, Prop>
{
  typedef any_completion_executor result_type;
};

#endif // !defined(BOOST_ASIO_HAS_DEDUCED_PREFER_MEMBER_TRAIT)

} // namespace traits

#endif // !defined(GENERATING_DOCUMENTATION)

#endif // defined(BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT)

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#if defined(BOOST_ASIO_HEADER_ONLY) \
  && !defined(BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT)
# include <boost/asio/impl/any_completion_executor.ipp>
#endif // defined(BOOST_ASIO_HEADER_ONLY)
       //   && !defined(BOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT)

#endif // BOOST_ASIO_ANY_COMPLETION_EXECUTOR_HPP
