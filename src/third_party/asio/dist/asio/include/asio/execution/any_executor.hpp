//
// execution/any_executor.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_EXECUTION_ANY_EXECUTOR_HPP
#define ASIO_EXECUTION_ANY_EXECUTOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <new>
#include <typeinfo>
#include "asio/detail/assert.hpp"
#include "asio/detail/atomic_count.hpp"
#include "asio/detail/cstddef.hpp"
#include "asio/detail/executor_function.hpp"
#include "asio/detail/memory.hpp"
#include "asio/detail/non_const_lvalue.hpp"
#include "asio/detail/scoped_ptr.hpp"
#include "asio/detail/type_traits.hpp"
#include "asio/detail/throw_exception.hpp"
#include "asio/execution/bad_executor.hpp"
#include "asio/execution/blocking.hpp"
#include "asio/execution/executor.hpp"
#include "asio/prefer.hpp"
#include "asio/query.hpp"
#include "asio/require.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

#if defined(GENERATING_DOCUMENTATION)

namespace execution {

/// Polymorphic executor wrapper.
template <typename... SupportableProperties>
class any_executor
{
public:
  /// Default constructor.
  any_executor() noexcept;

  /// Construct in an empty state. Equivalent effects to default constructor.
  any_executor(nullptr_t) noexcept;

  /// Copy constructor.
  any_executor(const any_executor& e) noexcept;

  /// Move constructor.
  any_executor(any_executor&& e) noexcept;

  /// Construct to point to the same target as another any_executor.
  template <class... OtherSupportableProperties>
    any_executor(any_executor<OtherSupportableProperties...> e);

  /// Construct to point to the same target as another any_executor.
  template <class... OtherSupportableProperties>
    any_executor(std::nothrow_t,
      any_executor<OtherSupportableProperties...> e) noexcept;

  /// Construct to point to the same target as another any_executor.
  any_executor(std::nothrow_t, const any_executor& e) noexcept;

  /// Construct to point to the same target as another any_executor.
  any_executor(std::nothrow_t, any_executor&& e) noexcept;

  /// Construct a polymorphic wrapper for the specified executor.
  template <typename Executor>
  any_executor(Executor e);

  /// Construct a polymorphic wrapper for the specified executor.
  template <typename Executor>
  any_executor(std::nothrow_t, Executor e) noexcept;

  /// Assignment operator.
  any_executor& operator=(const any_executor& e) noexcept;

  /// Move assignment operator.
  any_executor& operator=(any_executor&& e) noexcept;

  /// Assignment operator that sets the polymorphic wrapper to the empty state.
  any_executor& operator=(nullptr_t);

  /// Assignment operator to create a polymorphic wrapper for the specified
  /// executor.
  template <typename Executor>
  any_executor& operator=(Executor e);

  /// Destructor.
  ~any_executor();

  /// Swap targets with another polymorphic wrapper.
  void swap(any_executor& other) noexcept;

  /// Obtain a polymorphic wrapper with the specified property.
  /**
   * Do not call this function directly. It is intended for use with the
   * asio::require and asio::prefer customisation points.
   *
   * For example:
   * @code execution::any_executor<execution::blocking_t::possibly_t> ex = ...;
   * auto ex2 = asio::require(ex, execution::blocking.possibly); @endcode
   */
  template <typename Property>
  any_executor require(Property) const;

  /// Obtain a polymorphic wrapper with the specified property.
  /**
   * Do not call this function directly. It is intended for use with the
   * asio::prefer customisation point.
   *
   * For example:
   * @code execution::any_executor<execution::blocking_t::possibly_t> ex = ...;
   * auto ex2 = asio::prefer(ex, execution::blocking.possibly); @endcode
   */
  template <typename Property>
  any_executor prefer(Property) const;

  /// Obtain the value associated with the specified property.
  /**
   * Do not call this function directly. It is intended for use with the
   * asio::query customisation point.
   *
   * For example:
   * @code execution::any_executor<execution::occupancy_t> ex = ...;
   * size_t n = asio::query(ex, execution::occupancy); @endcode
   */
  template <typename Property>
  typename Property::polymorphic_query_result_type query(Property) const;

  /// Execute the function on the target executor.
  /**
   * Throws asio::bad_executor if the polymorphic wrapper has no target.
   */
  template <typename Function>
  void execute(Function&& f) const;

  /// Obtain the underlying execution context.
  /**
   * This function is provided for backward compatibility. It is automatically
   * defined when the @c SupportableProperties... list includes a property of
   * type <tt>execution::context_as<U></tt>, for some type <tt>U</tt>.
   */
  automatically_determined context() const;

  /// Determine whether the wrapper has a target executor.
  /**
   * @returns @c true if the polymorphic wrapper has a target executor,
   * otherwise false.
   */
  explicit operator bool() const noexcept;

  /// Get the type of the target executor.
  const type_info& target_type() const noexcept;

  /// Get a pointer to the target executor.
  template <typename Executor> Executor* target() noexcept;

  /// Get a pointer to the target executor.
  template <typename Executor> const Executor* target() const noexcept;
};

/// Equality operator.
/**
 * @relates any_executor
 */
template <typename... SupportableProperties>
bool operator==(const any_executor<SupportableProperties...>& a,
    const any_executor<SupportableProperties...>& b) noexcept;

/// Equality operator.
/**
 * @relates any_executor
 */
template <typename... SupportableProperties>
bool operator==(const any_executor<SupportableProperties...>& a,
    nullptr_t) noexcept;

/// Equality operator.
/**
 * @relates any_executor
 */
template <typename... SupportableProperties>
bool operator==(nullptr_t,
    const any_executor<SupportableProperties...>& b) noexcept;

/// Inequality operator.
/**
 * @relates any_executor
 */
template <typename... SupportableProperties>
bool operator!=(const any_executor<SupportableProperties...>& a,
    const any_executor<SupportableProperties...>& b) noexcept;

/// Inequality operator.
/**
 * @relates any_executor
 */
template <typename... SupportableProperties>
bool operator!=(const any_executor<SupportableProperties...>& a,
    nullptr_t) noexcept;

/// Inequality operator.
/**
 * @relates any_executor
 */
template <typename... SupportableProperties>
bool operator!=(nullptr_t,
    const any_executor<SupportableProperties...>& b) noexcept;

} // namespace execution

#else // defined(GENERATING_DOCUMENTATION)

namespace execution {

#if !defined(ASIO_EXECUTION_ANY_EXECUTOR_FWD_DECL)
#define ASIO_EXECUTION_ANY_EXECUTOR_FWD_DECL

template <typename... SupportableProperties>
class any_executor;

#endif // !defined(ASIO_EXECUTION_ANY_EXECUTOR_FWD_DECL)

template <typename U>
struct context_as_t;

namespace detail {

// Traits used to detect whether a property is requirable or preferable, taking
// into account that T::is_requirable or T::is_preferable may not not be well
// formed.

template <typename T, typename = void>
struct is_requirable : false_type {};

template <typename T>
struct is_requirable<T, enable_if_t<T::is_requirable>> : true_type {};

template <typename T, typename = void>
struct is_preferable : false_type {};

template <typename T>
struct is_preferable<T, enable_if_t<T::is_preferable>> : true_type {};

// Trait used to detect context_as property, for backward compatibility.

template <typename T>
struct is_context_as : false_type {};

template <typename U>
struct is_context_as<context_as_t<U>> : true_type {};

// Helper template to:
// - Check if a target can supply the supportable properties.
// - Find the first convertible-from-T property in the list.

template <std::size_t I, typename Props>
struct supportable_properties;

template <std::size_t I, typename Prop>
struct supportable_properties<I, void(Prop)>
{
  template <typename T>
  struct is_valid_target : integral_constant<bool,
      (
        is_requirable<Prop>::value
          ? can_require<T, Prop>::value
          : true
      )
      &&
      (
        is_preferable<Prop>::value
          ? can_prefer<T, Prop>::value
          : true
      )
      &&
      (
        !is_requirable<Prop>::value && !is_preferable<Prop>::value
          ? can_query<T, Prop>::value
          : true
      )
    >
  {
  };

  struct found
  {
    static constexpr bool value = true;
    typedef Prop type;
    typedef typename Prop::polymorphic_query_result_type query_result_type;
    static constexpr std::size_t index = I;
  };

  struct not_found
  {
    static constexpr bool value = false;
  };

  template <typename T>
  struct find_convertible_property :
      conditional_t<
        is_same<T, Prop>::value || is_convertible<T, Prop>::value,
        found,
        not_found
      > {};

  template <typename T>
  struct find_convertible_requirable_property :
      conditional_t<
        is_requirable<Prop>::value
          && (is_same<T, Prop>::value || is_convertible<T, Prop>::value),
        found,
        not_found
      > {};

  template <typename T>
  struct find_convertible_preferable_property :
      conditional_t<
        is_preferable<Prop>::value
          && (is_same<T, Prop>::value || is_convertible<T, Prop>::value),
        found,
        not_found
      > {};

  struct find_context_as_property :
      conditional_t<
        is_context_as<Prop>::value,
        found,
        not_found
      > {};
};

template <std::size_t I, typename Head, typename... Tail>
struct supportable_properties<I, void(Head, Tail...)>
{
  template <typename T>
  struct is_valid_target : integral_constant<bool,
      (
        supportable_properties<I,
          void(Head)>::template is_valid_target<T>::value
        &&
        supportable_properties<I + 1,
          void(Tail...)>::template is_valid_target<T>::value
      )
    >
  {
  };

  template <typename T>
  struct find_convertible_property :
      conditional_t<
        is_convertible<T, Head>::value,
        typename supportable_properties<I, void(Head)>::found,
        typename supportable_properties<I + 1,
            void(Tail...)>::template find_convertible_property<T>
      > {};

  template <typename T>
  struct find_convertible_requirable_property :
      conditional_t<
        is_requirable<Head>::value
          && is_convertible<T, Head>::value,
        typename supportable_properties<I, void(Head)>::found,
        typename supportable_properties<I + 1,
            void(Tail...)>::template find_convertible_requirable_property<T>
      > {};

  template <typename T>
  struct find_convertible_preferable_property :
      conditional_t<
        is_preferable<Head>::value
          && is_convertible<T, Head>::value,
        typename supportable_properties<I, void(Head)>::found,
        typename supportable_properties<I + 1,
            void(Tail...)>::template find_convertible_preferable_property<T>
      > {};

  struct find_context_as_property :
      conditional_t<
        is_context_as<Head>::value,
        typename supportable_properties<I, void(Head)>::found,
        typename supportable_properties<I + 1,
            void(Tail...)>::find_context_as_property
      > {};
};

template <typename T, typename Props>
struct is_valid_target_executor :
  conditional_t<
    is_executor<T>::value,
    typename supportable_properties<0, Props>::template is_valid_target<T>,
    false_type
  >
{
};

template <typename Props>
struct is_valid_target_executor<int, Props> : false_type
{
};

class shared_target_executor
{
public:
  template <typename E>
  shared_target_executor(E&& e, decay_t<E>*& target)
  {
    impl<decay_t<E>>* i =
      new impl<decay_t<E>>(static_cast<E&&>(e));
    target = &i->ex_;
    impl_ = i;
  }

  template <typename E>
  shared_target_executor(std::nothrow_t, E&& e, decay_t<E>*& target) noexcept
  {
    impl<decay_t<E>>* i =
      new (std::nothrow) impl<decay_t<E>>(static_cast<E&&>(e));
    target = i ? &i->ex_ : 0;
    impl_ = i;
  }

  shared_target_executor(const shared_target_executor& other) noexcept
    : impl_(other.impl_)
  {
    if (impl_)
      asio::detail::ref_count_up(impl_->ref_count_);
  }

  shared_target_executor(shared_target_executor&& other) noexcept
    : impl_(other.impl_)
  {
    other.impl_ = 0;
  }

  ~shared_target_executor()
  {
    if (impl_)
      if (asio::detail::ref_count_down(impl_->ref_count_))
        delete impl_;
  }

  void* get() const noexcept
  {
    return impl_ ? impl_->get() : 0;
  }

private:
  shared_target_executor& operator=(
      const shared_target_executor& other) = delete;

  shared_target_executor& operator=(
      shared_target_executor&& other) = delete;

  struct impl_base
  {
    impl_base() : ref_count_(1) {}
    virtual ~impl_base() {}
    virtual void* get() = 0;
    asio::detail::atomic_count ref_count_;
  };

  template <typename Executor>
  struct impl : impl_base
  {
    impl(Executor ex) : ex_(static_cast<Executor&&>(ex)) {}
    virtual void* get() { return &ex_; }
    Executor ex_;
  };

  impl_base* impl_;
};

class any_executor_base
{
public:
  any_executor_base() noexcept
    : object_fns_(0),
      target_(0),
      target_fns_(0)
  {
  }

  template <ASIO_EXECUTION_EXECUTOR Executor>
  any_executor_base(Executor ex, false_type)
    : target_fns_(target_fns_table<Executor>(
          any_executor_base::query_blocking(ex,
            can_query<const Executor&, const execution::blocking_t&>())
          == execution::blocking.always))
  {
    any_executor_base::construct_object(ex,
        integral_constant<bool,
          sizeof(Executor) <= sizeof(object_type)
            && alignment_of<Executor>::value <= alignment_of<object_type>::value
        >());
  }

  template <ASIO_EXECUTION_EXECUTOR Executor>
  any_executor_base(std::nothrow_t, Executor ex, false_type) noexcept
    : target_fns_(target_fns_table<Executor>(
          any_executor_base::query_blocking(ex,
            can_query<const Executor&, const execution::blocking_t&>())
          == execution::blocking.always))
  {
    any_executor_base::construct_object(std::nothrow, ex,
        integral_constant<bool,
          sizeof(Executor) <= sizeof(object_type)
            && alignment_of<Executor>::value <= alignment_of<object_type>::value
        >());
    if (target_ == 0)
    {
      object_fns_ = 0;
      target_fns_ = 0;
    }
  }

  template <ASIO_EXECUTION_EXECUTOR Executor>
  any_executor_base(Executor other, true_type)
    : object_fns_(object_fns_table<shared_target_executor>()),
      target_fns_(other.target_fns_)
  {
    Executor* p = 0;
    new (&object_) shared_target_executor(
        static_cast<Executor&&>(other), p);
    target_ = p->template target<void>();
  }

  template <ASIO_EXECUTION_EXECUTOR Executor>
  any_executor_base(std::nothrow_t,
      Executor other, true_type) noexcept
    : object_fns_(object_fns_table<shared_target_executor>()),
      target_fns_(other.target_fns_)
  {
    Executor* p = 0;
    new (&object_) shared_target_executor(
        std::nothrow, static_cast<Executor&&>(other), p);
    if (p)
      target_ = p->template target<void>();
    else
    {
      target_ = 0;
      object_fns_ = 0;
      target_fns_ = 0;
    }
  }

  any_executor_base(const any_executor_base& other) noexcept
  {
    if (!!other)
    {
      object_fns_ = other.object_fns_;
      target_fns_ = other.target_fns_;
      object_fns_->copy(*this, other);
    }
    else
    {
      object_fns_ = 0;
      target_ = 0;
      target_fns_ = 0;
    }
  }

  ~any_executor_base() noexcept
  {
    if (!!*this)
      object_fns_->destroy(*this);
  }

  any_executor_base& operator=(
      const any_executor_base& other) noexcept
  {
    if (this != &other)
    {
      if (!!*this)
        object_fns_->destroy(*this);
      if (!!other)
      {
        object_fns_ = other.object_fns_;
        target_fns_ = other.target_fns_;
        object_fns_->copy(*this, other);
      }
      else
      {
        object_fns_ = 0;
        target_ = 0;
        target_fns_ = 0;
      }
    }
    return *this;
  }

  any_executor_base& operator=(nullptr_t) noexcept
  {
    if (target_)
      object_fns_->destroy(*this);
    target_ = 0;
    object_fns_ = 0;
    target_fns_ = 0;
    return *this;
  }

  any_executor_base(any_executor_base&& other) noexcept
  {
    if (other.target_)
    {
      object_fns_ = other.object_fns_;
      target_fns_ = other.target_fns_;
      other.object_fns_ = 0;
      other.target_fns_ = 0;
      object_fns_->move(*this, other);
      other.target_ = 0;
    }
    else
    {
      object_fns_ = 0;
      target_ = 0;
      target_fns_ = 0;
    }
  }

  any_executor_base& operator=(
      any_executor_base&& other) noexcept
  {
    if (this != &other)
    {
      if (!!*this)
        object_fns_->destroy(*this);
      if (!!other)
      {
        object_fns_ = other.object_fns_;
        target_fns_ = other.target_fns_;
        other.object_fns_ = 0;
        other.target_fns_ = 0;
        object_fns_->move(*this, other);
        other.target_ = 0;
      }
      else
      {
        object_fns_ = 0;
        target_ = 0;
        target_fns_ = 0;
      }
    }
    return *this;
  }

  void swap(any_executor_base& other) noexcept
  {
    if (this != &other)
    {
      any_executor_base tmp(static_cast<any_executor_base&&>(other));
      other = static_cast<any_executor_base&&>(*this);
      *this = static_cast<any_executor_base&&>(tmp);
    }
  }

  template <typename F>
  void execute(F&& f) const
  {
    if (target_)
    {
      if (target_fns_->blocking_execute != 0)
      {
        asio::detail::non_const_lvalue<F> f2(f);
        target_fns_->blocking_execute(*this, function_view(f2.value));
      }
      else
      {
        target_fns_->execute(*this,
            function(static_cast<F&&>(f), std::allocator<void>()));
      }
    }
    else
    {
      bad_executor ex;
      asio::detail::throw_exception(ex);
    }
  }

  template <typename Executor>
  Executor* target()
  {
    return target_ && (is_same<Executor, void>::value
        || target_fns_->target_type() == target_type_ex<Executor>())
      ? static_cast<Executor*>(target_) : 0;
  }

  template <typename Executor>
  const Executor* target() const
  {
    return target_ && (is_same<Executor, void>::value
        || target_fns_->target_type() == target_type_ex<Executor>())
      ? static_cast<const Executor*>(target_) : 0;
  }

#if !defined(ASIO_NO_TYPEID)
  const std::type_info& target_type() const
  {
    return target_ ? target_fns_->target_type() : typeid(void);
  }
#else // !defined(ASIO_NO_TYPEID)
  const void* target_type() const
  {
    return target_ ? target_fns_->target_type() : 0;
  }
#endif // !defined(ASIO_NO_TYPEID)

  struct unspecified_bool_type_t {};
  typedef void (*unspecified_bool_type)(unspecified_bool_type_t);
  static void unspecified_bool_true(unspecified_bool_type_t) {}

  operator unspecified_bool_type() const noexcept
  {
    return target_ ? &any_executor_base::unspecified_bool_true : 0;
  }

  bool operator!() const noexcept
  {
    return target_ == 0;
  }

protected:
  bool equality_helper(const any_executor_base& other) const noexcept
  {
    if (target_ == other.target_)
      return true;
    if (target_ && !other.target_)
      return false;
    if (!target_ && other.target_)
      return false;
    if (target_fns_ != other.target_fns_)
      return false;
    return target_fns_->equal(*this, other);
  }

  template <typename Ex>
  Ex& object()
  {
    return *static_cast<Ex*>(static_cast<void*>(&object_));
  }

  template <typename Ex>
  const Ex& object() const
  {
    return *static_cast<const Ex*>(static_cast<const void*>(&object_));
  }

  struct object_fns
  {
    void (*destroy)(any_executor_base&);
    void (*copy)(any_executor_base&, const any_executor_base&);
    void (*move)(any_executor_base&, any_executor_base&);
    const void* (*target)(const any_executor_base&);
  };

  static void destroy_shared(any_executor_base& ex)
  {
    typedef shared_target_executor type;
    ex.object<type>().~type();
  }

  static void copy_shared(any_executor_base& ex1, const any_executor_base& ex2)
  {
    typedef shared_target_executor type;
    new (&ex1.object_) type(ex2.object<type>());
    ex1.target_ = ex2.target_;
  }

  static void move_shared(any_executor_base& ex1, any_executor_base& ex2)
  {
    typedef shared_target_executor type;
    new (&ex1.object_) type(static_cast<type&&>(ex2.object<type>()));
    ex1.target_ = ex2.target_;
    ex2.object<type>().~type();
  }

  static const void* target_shared(const any_executor_base& ex)
  {
    typedef shared_target_executor type;
    return ex.object<type>().get();
  }

  template <typename Obj>
  static const object_fns* object_fns_table(
      enable_if_t<
        is_same<Obj, shared_target_executor>::value
      >* = 0)
  {
    static const object_fns fns =
    {
      &any_executor_base::destroy_shared,
      &any_executor_base::copy_shared,
      &any_executor_base::move_shared,
      &any_executor_base::target_shared
    };
    return &fns;
  }

  template <typename Obj>
  static void destroy_object(any_executor_base& ex)
  {
    ex.object<Obj>().~Obj();
  }

  template <typename Obj>
  static void copy_object(any_executor_base& ex1, const any_executor_base& ex2)
  {
    new (&ex1.object_) Obj(ex2.object<Obj>());
    ex1.target_ = &ex1.object<Obj>();
  }

  template <typename Obj>
  static void move_object(any_executor_base& ex1, any_executor_base& ex2)
  {
    new (&ex1.object_) Obj(static_cast<Obj&&>(ex2.object<Obj>()));
    ex1.target_ = &ex1.object<Obj>();
    ex2.object<Obj>().~Obj();
  }

  template <typename Obj>
  static const void* target_object(const any_executor_base& ex)
  {
    return &ex.object<Obj>();
  }

  template <typename Obj>
  static const object_fns* object_fns_table(
      enable_if_t<
        !is_same<Obj, void>::value
          && !is_same<Obj, shared_target_executor>::value
      >* = 0)
  {
    static const object_fns fns =
    {
      &any_executor_base::destroy_object<Obj>,
      &any_executor_base::copy_object<Obj>,
      &any_executor_base::move_object<Obj>,
      &any_executor_base::target_object<Obj>
    };
    return &fns;
  }

  typedef asio::detail::executor_function function;
  typedef asio::detail::executor_function_view function_view;

  struct target_fns
  {
#if !defined(ASIO_NO_TYPEID)
    const std::type_info& (*target_type)();
#else // !defined(ASIO_NO_TYPEID)
    const void* (*target_type)();
#endif // !defined(ASIO_NO_TYPEID)
    bool (*equal)(const any_executor_base&, const any_executor_base&);
    void (*execute)(const any_executor_base&, function&&);
    void (*blocking_execute)(const any_executor_base&, function_view);
  };

#if !defined(ASIO_NO_TYPEID)
  template <typename Ex>
  static const std::type_info& target_type_ex()
  {
    return typeid(Ex);
  }
#else // !defined(ASIO_NO_TYPEID)
  template <typename Ex>
  static const void* target_type_ex()
  {
    static int unique_id;
    return &unique_id;
  }
#endif // !defined(ASIO_NO_TYPEID)

  template <typename Ex>
  static bool equal_ex(const any_executor_base& ex1,
      const any_executor_base& ex2)
  {
    const Ex* p1 = ex1.target<Ex>();
    const Ex* p2 = ex2.target<Ex>();
    ASIO_ASSUME(p1 != 0 && p2 != 0);
    return *p1 == *p2;
  }

  template <typename Ex>
  static void execute_ex(const any_executor_base& ex, function&& f)
  {
    const Ex* p = ex.target<Ex>();
    ASIO_ASSUME(p != 0);
    p->execute(static_cast<function&&>(f));
  }

  template <typename Ex>
  static void blocking_execute_ex(const any_executor_base& ex, function_view f)
  {
    const Ex* p = ex.target<Ex>();
    ASIO_ASSUME(p != 0);
    p->execute(f);
  }

  template <typename Ex>
  static const target_fns* target_fns_table(bool is_always_blocking,
      enable_if_t<
        !is_same<Ex, void>::value
      >* = 0)
  {
    static const target_fns fns_with_execute =
    {
      &any_executor_base::target_type_ex<Ex>,
      &any_executor_base::equal_ex<Ex>,
      &any_executor_base::execute_ex<Ex>,
      0
    };

    static const target_fns fns_with_blocking_execute =
    {
      &any_executor_base::target_type_ex<Ex>,
      &any_executor_base::equal_ex<Ex>,
      0,
      &any_executor_base::blocking_execute_ex<Ex>
    };

    return is_always_blocking ? &fns_with_blocking_execute : &fns_with_execute;
  }

#if defined(ASIO_MSVC)
# pragma warning (push)
# pragma warning (disable:4702)
#endif // defined(ASIO_MSVC)

  static void query_fn_void(void*, const void*, const void*)
  {
    bad_executor ex;
    asio::detail::throw_exception(ex);
  }

  template <typename Ex, class Prop>
  static void query_fn_non_void(void*, const void* ex, const void* prop,
      enable_if_t<
        asio::can_query<const Ex&, const Prop&>::value
          && is_same<typename Prop::polymorphic_query_result_type, void>::value
      >*)
  {
    asio::query(*static_cast<const Ex*>(ex),
        *static_cast<const Prop*>(prop));
  }

  template <typename Ex, class Prop>
  static void query_fn_non_void(void*, const void*, const void*,
      enable_if_t<
        !asio::can_query<const Ex&, const Prop&>::value
          && is_same<typename Prop::polymorphic_query_result_type, void>::value
      >*)
  {
  }

  template <typename Ex, class Prop>
  static void query_fn_non_void(void* result, const void* ex, const void* prop,
      enable_if_t<
        asio::can_query<const Ex&, const Prop&>::value
          && !is_same<typename Prop::polymorphic_query_result_type, void>::value
          && is_reference<typename Prop::polymorphic_query_result_type>::value
      >*)
  {
    *static_cast<remove_reference_t<
      typename Prop::polymorphic_query_result_type>**>(result)
        = &static_cast<typename Prop::polymorphic_query_result_type>(
            asio::query(*static_cast<const Ex*>(ex),
              *static_cast<const Prop*>(prop)));
  }

  template <typename Ex, class Prop>
  static void query_fn_non_void(void*, const void*, const void*,
      enable_if_t<
        !asio::can_query<const Ex&, const Prop&>::value
          && !is_same<typename Prop::polymorphic_query_result_type, void>::value
          && is_reference<typename Prop::polymorphic_query_result_type>::value
      >*)
  {
    std::terminate(); // Combination should not be possible.
  }

  template <typename Ex, class Prop>
  static void query_fn_non_void(void* result, const void* ex, const void* prop,
      enable_if_t<
        asio::can_query<const Ex&, const Prop&>::value
          && !is_same<typename Prop::polymorphic_query_result_type, void>::value
          && is_scalar<typename Prop::polymorphic_query_result_type>::value
      >*)
  {
    *static_cast<typename Prop::polymorphic_query_result_type*>(result)
      = static_cast<typename Prop::polymorphic_query_result_type>(
          asio::query(*static_cast<const Ex*>(ex),
            *static_cast<const Prop*>(prop)));
  }

  template <typename Ex, class Prop>
  static void query_fn_non_void(void* result, const void*, const void*,
      enable_if_t<
        !asio::can_query<const Ex&, const Prop&>::value
          && !is_same<typename Prop::polymorphic_query_result_type, void>::value
          && is_scalar<typename Prop::polymorphic_query_result_type>::value
      >*)
  {
    *static_cast<typename Prop::polymorphic_query_result_type*>(result)
      = typename Prop::polymorphic_query_result_type();
  }

  template <typename Ex, class Prop>
  static void query_fn_non_void(void* result, const void* ex, const void* prop,
      enable_if_t<
        asio::can_query<const Ex&, const Prop&>::value
          && !is_same<typename Prop::polymorphic_query_result_type, void>::value
          && !is_reference<typename Prop::polymorphic_query_result_type>::value
          && !is_scalar<typename Prop::polymorphic_query_result_type>::value
      >*)
  {
    *static_cast<typename Prop::polymorphic_query_result_type**>(result)
      = new typename Prop::polymorphic_query_result_type(
          asio::query(*static_cast<const Ex*>(ex),
            *static_cast<const Prop*>(prop)));
  }

  template <typename Ex, class Prop>
  static void query_fn_non_void(void* result, const void*, const void*, ...)
  {
    *static_cast<typename Prop::polymorphic_query_result_type**>(result)
      = new typename Prop::polymorphic_query_result_type();
  }

  template <typename Ex, class Prop>
  static void query_fn_impl(void* result, const void* ex, const void* prop,
      enable_if_t<
        is_same<Ex, void>::value
      >*)
  {
    query_fn_void(result, ex, prop);
  }

  template <typename Ex, class Prop>
  static void query_fn_impl(void* result, const void* ex, const void* prop,
      enable_if_t<
        !is_same<Ex, void>::value
      >*)
  {
    query_fn_non_void<Ex, Prop>(result, ex, prop, 0);
  }

  template <typename Ex, class Prop>
  static void query_fn(void* result, const void* ex, const void* prop)
  {
    query_fn_impl<Ex, Prop>(result, ex, prop, 0);
  }

  template <typename Poly, typename Ex, class Prop>
  static Poly require_fn_impl(const void*, const void*,
      enable_if_t<
        is_same<Ex, void>::value
      >*)
  {
    bad_executor ex;
    asio::detail::throw_exception(ex);
    return Poly();
  }

  template <typename Poly, typename Ex, class Prop>
  static Poly require_fn_impl(const void* ex, const void* prop,
      enable_if_t<
        !is_same<Ex, void>::value && Prop::is_requirable
      >*)
  {
    return asio::require(*static_cast<const Ex*>(ex),
        *static_cast<const Prop*>(prop));
  }

  template <typename Poly, typename Ex, class Prop>
  static Poly require_fn_impl(const void*, const void*, ...)
  {
    return Poly();
  }

  template <typename Poly, typename Ex, class Prop>
  static Poly require_fn(const void* ex, const void* prop)
  {
    return require_fn_impl<Poly, Ex, Prop>(ex, prop, 0);
  }

  template <typename Poly, typename Ex, class Prop>
  static Poly prefer_fn_impl(const void*, const void*,
      enable_if_t<
        is_same<Ex, void>::value
      >*)
  {
    bad_executor ex;
    asio::detail::throw_exception(ex);
    return Poly();
  }

  template <typename Poly, typename Ex, class Prop>
  static Poly prefer_fn_impl(const void* ex, const void* prop,
      enable_if_t<
        !is_same<Ex, void>::value && Prop::is_preferable
      >*)
  {
    return asio::prefer(*static_cast<const Ex*>(ex),
        *static_cast<const Prop*>(prop));
  }

  template <typename Poly, typename Ex, class Prop>
  static Poly prefer_fn_impl(const void*, const void*, ...)
  {
    return Poly();
  }

  template <typename Poly, typename Ex, class Prop>
  static Poly prefer_fn(const void* ex, const void* prop)
  {
    return prefer_fn_impl<Poly, Ex, Prop>(ex, prop, 0);
  }

  template <typename Poly>
  struct prop_fns
  {
    void (*query)(void*, const void*, const void*);
    Poly (*require)(const void*, const void*);
    Poly (*prefer)(const void*, const void*);
  };

#if defined(ASIO_MSVC)
# pragma warning (pop)
#endif // defined(ASIO_MSVC)

private:
  template <typename Executor>
  static execution::blocking_t query_blocking(const Executor& ex, true_type)
  {
    return asio::query(ex, execution::blocking);
  }

  template <typename Executor>
  static execution::blocking_t query_blocking(const Executor&, false_type)
  {
    return execution::blocking_t();
  }

  template <typename Executor>
  void construct_object(Executor& ex, true_type)
  {
    object_fns_ = object_fns_table<Executor>();
    target_ = new (&object_) Executor(static_cast<Executor&&>(ex));
  }

  template <typename Executor>
  void construct_object(Executor& ex, false_type)
  {
    object_fns_ = object_fns_table<shared_target_executor>();
    Executor* p = 0;
    new (&object_) shared_target_executor(
        static_cast<Executor&&>(ex), p);
    target_ = p;
  }

  template <typename Executor>
  void construct_object(std::nothrow_t,
      Executor& ex, true_type) noexcept
  {
    object_fns_ = object_fns_table<Executor>();
    target_ = new (&object_) Executor(static_cast<Executor&&>(ex));
  }

  template <typename Executor>
  void construct_object(std::nothrow_t,
      Executor& ex, false_type) noexcept
  {
    object_fns_ = object_fns_table<shared_target_executor>();
    Executor* p = 0;
    new (&object_) shared_target_executor(
        std::nothrow, static_cast<Executor&&>(ex), p);
    target_ = p;
  }

/*private:*/public:
//  template <typename...> friend class any_executor;

  typedef aligned_storage<
      sizeof(asio::detail::shared_ptr<void>) + sizeof(void*),
      alignment_of<asio::detail::shared_ptr<void>>::value
    >::type object_type;

  object_type object_;
  const object_fns* object_fns_;
  void* target_;
  const target_fns* target_fns_;
};

template <typename Derived, typename Property, typename = void>
struct any_executor_context
{
};

#if !defined(ASIO_NO_TS_EXECUTORS)

template <typename Derived, typename Property>
struct any_executor_context<Derived, Property, enable_if_t<Property::value>>
{
  typename Property::query_result_type context() const
  {
    return static_cast<const Derived*>(this)->query(typename Property::type());
  }
};

#endif // !defined(ASIO_NO_TS_EXECUTORS)

} // namespace detail

template <>
class any_executor<> : public detail::any_executor_base
{
public:
  any_executor() noexcept
    : detail::any_executor_base()
  {
  }

  any_executor(nullptr_t) noexcept
    : detail::any_executor_base()
  {
  }

  template <typename Executor>
  any_executor(Executor ex,
      enable_if_t<
        conditional_t<
          !is_same<Executor, any_executor>::value
            && !is_base_of<detail::any_executor_base, Executor>::value,
          is_executor<Executor>,
          false_type
        >::value
      >* = 0)
    : detail::any_executor_base(
        static_cast<Executor&&>(ex), false_type())
  {
  }

  template <typename Executor>
  any_executor(std::nothrow_t, Executor ex,
      enable_if_t<
        conditional_t<
          !is_same<Executor, any_executor>::value
            && !is_base_of<detail::any_executor_base, Executor>::value,
          is_executor<Executor>,
          false_type
        >::value
      >* = 0) noexcept
    : detail::any_executor_base(std::nothrow,
        static_cast<Executor&&>(ex), false_type())
  {
  }

  template <typename... OtherSupportableProperties>
  any_executor(any_executor<OtherSupportableProperties...> other)
    : detail::any_executor_base(
        static_cast<const detail::any_executor_base&>(other))
  {
  }

  template <typename... OtherSupportableProperties>
  any_executor(std::nothrow_t,
      any_executor<OtherSupportableProperties...> other) noexcept
    : detail::any_executor_base(
        static_cast<const detail::any_executor_base&>(other))
  {
  }

  any_executor(const any_executor& other) noexcept
    : detail::any_executor_base(
        static_cast<const detail::any_executor_base&>(other))
  {
  }

  any_executor(std::nothrow_t, const any_executor& other) noexcept
    : detail::any_executor_base(
        static_cast<const detail::any_executor_base&>(other))
  {
  }

  any_executor& operator=(const any_executor& other) noexcept
  {
    if (this != &other)
    {
      detail::any_executor_base::operator=(
          static_cast<const detail::any_executor_base&>(other));
    }
    return *this;
  }

  any_executor& operator=(nullptr_t p) noexcept
  {
    detail::any_executor_base::operator=(p);
    return *this;
  }

  any_executor(any_executor&& other) noexcept
    : detail::any_executor_base(
        static_cast<any_executor_base&&>(
          static_cast<any_executor_base&>(other)))
  {
  }

  any_executor(std::nothrow_t, any_executor&& other) noexcept
    : detail::any_executor_base(
        static_cast<any_executor_base&&>(
          static_cast<any_executor_base&>(other)))
  {
  }

  any_executor& operator=(any_executor&& other) noexcept
  {
    if (this != &other)
    {
      detail::any_executor_base::operator=(
          static_cast<detail::any_executor_base&&>(
            static_cast<detail::any_executor_base&>(other)));
    }
    return *this;
  }

  void swap(any_executor& other) noexcept
  {
    detail::any_executor_base::swap(
        static_cast<detail::any_executor_base&>(other));
  }

  using detail::any_executor_base::execute;
  using detail::any_executor_base::target;
  using detail::any_executor_base::target_type;
  using detail::any_executor_base::operator unspecified_bool_type;
  using detail::any_executor_base::operator!;

  bool equality_helper(const any_executor& other) const noexcept
  {
    return any_executor_base::equality_helper(other);
  }

  template <typename AnyExecutor1, typename AnyExecutor2>
  friend enable_if_t<
    is_base_of<any_executor, AnyExecutor1>::value
      || is_base_of<any_executor, AnyExecutor2>::value,
    bool
  > operator==(const AnyExecutor1& a,
      const AnyExecutor2& b) noexcept
  {
    return static_cast<const any_executor&>(a).equality_helper(b);
  }

  template <typename AnyExecutor>
  friend enable_if_t<
    is_same<AnyExecutor, any_executor>::value,
    bool
  > operator==(const AnyExecutor& a, nullptr_t) noexcept
  {
    return !a;
  }

  template <typename AnyExecutor>
  friend enable_if_t<
    is_same<AnyExecutor, any_executor>::value,
    bool
  > operator==(nullptr_t, const AnyExecutor& b) noexcept
  {
    return !b;
  }

  template <typename AnyExecutor1, typename AnyExecutor2>
  friend enable_if_t<
    is_base_of<any_executor, AnyExecutor1>::value
      || is_base_of<any_executor, AnyExecutor2>::value,
    bool
  > operator!=(const AnyExecutor1& a,
      const AnyExecutor2& b) noexcept
  {
    return !static_cast<const any_executor&>(a).equality_helper(b);
  }

  template <typename AnyExecutor>
  friend enable_if_t<
    is_same<AnyExecutor, any_executor>::value,
    bool
  > operator!=(const AnyExecutor& a, nullptr_t) noexcept
  {
    return !!a;
  }

  template <typename AnyExecutor>
  friend enable_if_t<
    is_same<AnyExecutor, any_executor>::value,
    bool
  > operator!=(nullptr_t, const AnyExecutor& b) noexcept
  {
    return !!b;
  }
};

inline void swap(any_executor<>& a, any_executor<>& b) noexcept
{
  return a.swap(b);
}

template <typename... SupportableProperties>
class any_executor :
  public detail::any_executor_base,
  public detail::any_executor_context<
    any_executor<SupportableProperties...>,
      typename detail::supportable_properties<
        0, void(SupportableProperties...)>::find_context_as_property>
{
public:
  any_executor() noexcept
    : detail::any_executor_base(),
      prop_fns_(prop_fns_table<void>())
  {
  }

  any_executor(nullptr_t) noexcept
    : detail::any_executor_base(),
      prop_fns_(prop_fns_table<void>())
  {
  }

  template <typename Executor>
  any_executor(Executor ex,
      enable_if_t<
        conditional_t<
          !is_same<Executor, any_executor>::value
            && !is_base_of<detail::any_executor_base, Executor>::value,
          detail::is_valid_target_executor<
            Executor, void(SupportableProperties...)>,
          false_type
        >::value
      >* = 0)
    : detail::any_executor_base(
        static_cast<Executor&&>(ex), false_type()),
      prop_fns_(prop_fns_table<Executor>())
  {
  }

  template <typename Executor>
  any_executor(std::nothrow_t, Executor ex,
      enable_if_t<
        conditional_t<
          !is_same<Executor, any_executor>::value
            && !is_base_of<detail::any_executor_base, Executor>::value,
          detail::is_valid_target_executor<
            Executor, void(SupportableProperties...)>,
          false_type
        >::value
      >* = 0) noexcept
    : detail::any_executor_base(std::nothrow,
        static_cast<Executor&&>(ex), false_type()),
      prop_fns_(prop_fns_table<Executor>())
  {
    if (this->template target<void>() == 0)
      prop_fns_ = prop_fns_table<void>();
  }

  template <typename... OtherSupportableProperties>
  any_executor(any_executor<OtherSupportableProperties...> other,
      enable_if_t<
        conditional_t<
          !is_same<
            any_executor<OtherSupportableProperties...>,
            any_executor
          >::value,
          typename detail::supportable_properties<
            0, void(SupportableProperties...)>::template is_valid_target<
              any_executor<OtherSupportableProperties...>>,
          false_type
        >::value
      >* = 0)
    : detail::any_executor_base(
        static_cast<any_executor<OtherSupportableProperties...>&&>(other),
        true_type()),
      prop_fns_(prop_fns_table<any_executor<OtherSupportableProperties...>>())
  {
  }

  template <typename... OtherSupportableProperties>
  any_executor(std::nothrow_t,
      any_executor<OtherSupportableProperties...> other,
      enable_if_t<
        conditional_t<
          !is_same<
            any_executor<OtherSupportableProperties...>,
            any_executor
          >::value,
          typename detail::supportable_properties<
            0, void(SupportableProperties...)>::template is_valid_target<
              any_executor<OtherSupportableProperties...>>,
          false_type
        >::value
      >* = 0) noexcept
    : detail::any_executor_base(std::nothrow,
        static_cast<any_executor<OtherSupportableProperties...>&&>(other),
        true_type()),
      prop_fns_(prop_fns_table<any_executor<OtherSupportableProperties...>>())
  {
    if (this->template target<void>() == 0)
      prop_fns_ = prop_fns_table<void>();
  }

  any_executor(const any_executor& other) noexcept
    : detail::any_executor_base(
        static_cast<const detail::any_executor_base&>(other)),
      prop_fns_(other.prop_fns_)
  {
  }

  any_executor(std::nothrow_t, const any_executor& other) noexcept
    : detail::any_executor_base(
        static_cast<const detail::any_executor_base&>(other)),
      prop_fns_(other.prop_fns_)
  {
  }

  any_executor& operator=(const any_executor& other) noexcept
  {
    if (this != &other)
    {
      prop_fns_ = other.prop_fns_;
      detail::any_executor_base::operator=(
          static_cast<const detail::any_executor_base&>(other));
    }
    return *this;
  }

  any_executor& operator=(nullptr_t p) noexcept
  {
    prop_fns_ = prop_fns_table<void>();
    detail::any_executor_base::operator=(p);
    return *this;
  }

  any_executor(any_executor&& other) noexcept
    : detail::any_executor_base(
        static_cast<any_executor_base&&>(
          static_cast<any_executor_base&>(other))),
      prop_fns_(other.prop_fns_)
  {
    other.prop_fns_ = prop_fns_table<void>();
  }

  any_executor(std::nothrow_t, any_executor&& other) noexcept
    : detail::any_executor_base(
        static_cast<any_executor_base&&>(
          static_cast<any_executor_base&>(other))),
      prop_fns_(other.prop_fns_)
  {
    other.prop_fns_ = prop_fns_table<void>();
  }

  any_executor& operator=(any_executor&& other) noexcept
  {
    if (this != &other)
    {
      prop_fns_ = other.prop_fns_;
      detail::any_executor_base::operator=(
          static_cast<detail::any_executor_base&&>(
            static_cast<detail::any_executor_base&>(other)));
    }
    return *this;
  }

  void swap(any_executor& other) noexcept
  {
    if (this != &other)
    {
      detail::any_executor_base::swap(
          static_cast<detail::any_executor_base&>(other));
      const prop_fns<any_executor>* tmp_prop_fns = other.prop_fns_;
      other.prop_fns_ = prop_fns_;
      prop_fns_ = tmp_prop_fns;
    }
  }

  using detail::any_executor_base::execute;
  using detail::any_executor_base::target;
  using detail::any_executor_base::target_type;
  using detail::any_executor_base::operator unspecified_bool_type;
  using detail::any_executor_base::operator!;

  bool equality_helper(const any_executor& other) const noexcept
  {
    return any_executor_base::equality_helper(other);
  }

  template <typename AnyExecutor1, typename AnyExecutor2>
  friend enable_if_t<
    is_base_of<any_executor, AnyExecutor1>::value
      || is_base_of<any_executor, AnyExecutor2>::value,
    bool
  > operator==(const AnyExecutor1& a,
      const AnyExecutor2& b) noexcept
  {
    return static_cast<const any_executor&>(a).equality_helper(b);
  }

  template <typename AnyExecutor>
  friend enable_if_t<
    is_same<AnyExecutor, any_executor>::value,
    bool
  > operator==(const AnyExecutor& a, nullptr_t) noexcept
  {
    return !a;
  }

  template <typename AnyExecutor>
  friend enable_if_t<
    is_same<AnyExecutor, any_executor>::value,
    bool
  > operator==(nullptr_t, const AnyExecutor& b) noexcept
  {
    return !b;
  }

  template <typename AnyExecutor1, typename AnyExecutor2>
  friend enable_if_t<
    is_base_of<any_executor, AnyExecutor1>::value
      || is_base_of<any_executor, AnyExecutor2>::value,
    bool
  > operator!=(const AnyExecutor1& a,
      const AnyExecutor2& b) noexcept
  {
    return !static_cast<const any_executor&>(a).equality_helper(b);
  }

  template <typename AnyExecutor>
  friend enable_if_t<
    is_same<AnyExecutor, any_executor>::value,
    bool
  > operator!=(const AnyExecutor& a, nullptr_t) noexcept
  {
    return !!a;
  }

  template <typename AnyExecutor>
  friend enable_if_t<
    is_same<AnyExecutor, any_executor>::value,
    bool
  > operator!=(nullptr_t, const AnyExecutor& b) noexcept
  {
    return !!b;
  }

  template <typename T>
  struct find_convertible_property :
      detail::supportable_properties<
        0, void(SupportableProperties...)>::template
          find_convertible_property<T> {};

  template <typename Property>
  void query(const Property& p,
      enable_if_t<
        is_same<
          typename find_convertible_property<Property>::query_result_type,
          void
        >::value
      >* = 0) const
  {
    if (!target_)
    {
      bad_executor ex;
      asio::detail::throw_exception(ex);
    }
    typedef find_convertible_property<Property> found;
    prop_fns_[found::index].query(0, object_fns_->target(*this),
        &static_cast<const typename found::type&>(p));
  }

  template <typename Property>
  typename find_convertible_property<Property>::query_result_type
  query(const Property& p,
      enable_if_t<
        !is_same<
          typename find_convertible_property<Property>::query_result_type,
          void
        >::value
        &&
        is_reference<
          typename find_convertible_property<Property>::query_result_type
        >::value
      >* = 0) const
  {
    if (!target_)
    {
      bad_executor ex;
      asio::detail::throw_exception(ex);
    }
    typedef find_convertible_property<Property> found;
    remove_reference_t<typename found::query_result_type>* result = 0;
    prop_fns_[found::index].query(&result, object_fns_->target(*this),
        &static_cast<const typename found::type&>(p));
    return *result;
  }

  template <typename Property>
  typename find_convertible_property<Property>::query_result_type
  query(const Property& p,
      enable_if_t<
        !is_same<
          typename find_convertible_property<Property>::query_result_type,
          void
        >::value
        &&
        is_scalar<
          typename find_convertible_property<Property>::query_result_type
        >::value
      >* = 0) const
  {
    if (!target_)
    {
      bad_executor ex;
      asio::detail::throw_exception(ex);
    }
    typedef find_convertible_property<Property> found;
    typename found::query_result_type result;
    prop_fns_[found::index].query(&result, object_fns_->target(*this),
        &static_cast<const typename found::type&>(p));
    return result;
  }

  template <typename Property>
  typename find_convertible_property<Property>::query_result_type
  query(const Property& p,
      enable_if_t<
        !is_same<
          typename find_convertible_property<Property>::query_result_type,
          void
        >::value
        &&
        !is_reference<
          typename find_convertible_property<Property>::query_result_type
        >::value
        &&
        !is_scalar<
          typename find_convertible_property<Property>::query_result_type
        >::value
      >* = 0) const
  {
    if (!target_)
    {
      bad_executor ex;
      asio::detail::throw_exception(ex);
    }
    typedef find_convertible_property<Property> found;
    typename found::query_result_type* result;
    prop_fns_[found::index].query(&result, object_fns_->target(*this),
        &static_cast<const typename found::type&>(p));
    return *asio::detail::scoped_ptr<
      typename found::query_result_type>(result);
  }

  template <typename T>
  struct find_convertible_requirable_property :
      detail::supportable_properties<
        0, void(SupportableProperties...)>::template
          find_convertible_requirable_property<T> {};

  template <typename Property>
  any_executor require(const Property& p,
      enable_if_t<
        find_convertible_requirable_property<Property>::value
      >* = 0) const
  {
    if (!target_)
    {
      bad_executor ex;
      asio::detail::throw_exception(ex);
    }
    typedef find_convertible_requirable_property<Property> found;
    return prop_fns_[found::index].require(object_fns_->target(*this),
        &static_cast<const typename found::type&>(p));
  }

  template <typename T>
  struct find_convertible_preferable_property :
      detail::supportable_properties<
        0, void(SupportableProperties...)>::template
          find_convertible_preferable_property<T> {};

  template <typename Property>
  any_executor prefer(const Property& p,
      enable_if_t<
        find_convertible_preferable_property<Property>::value
      >* = 0) const
  {
    if (!target_)
    {
      bad_executor ex;
      asio::detail::throw_exception(ex);
    }
    typedef find_convertible_preferable_property<Property> found;
    return prop_fns_[found::index].prefer(object_fns_->target(*this),
        &static_cast<const typename found::type&>(p));
  }

//private:
  template <typename Ex>
  static const prop_fns<any_executor>* prop_fns_table()
  {
    static const prop_fns<any_executor> fns[] =
    {
      {
        &detail::any_executor_base::query_fn<
            Ex, SupportableProperties>,
        &detail::any_executor_base::require_fn<
            any_executor, Ex, SupportableProperties>,
        &detail::any_executor_base::prefer_fn<
            any_executor, Ex, SupportableProperties>
      }...
    };
    return fns;
  }

  const prop_fns<any_executor>* prop_fns_;
};

template <typename... SupportableProperties>
inline void swap(any_executor<SupportableProperties...>& a,
    any_executor<SupportableProperties...>& b) noexcept
{
  return a.swap(b);
}

} // namespace execution
namespace traits {

#if !defined(ASIO_HAS_DEDUCED_EQUALITY_COMPARABLE_TRAIT)

template <typename... SupportableProperties>
struct equality_comparable<execution::any_executor<SupportableProperties...>>
{
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = true;
};

#endif // !defined(ASIO_HAS_DEDUCED_EQUALITY_COMPARABLE_TRAIT)

#if !defined(ASIO_HAS_DEDUCED_EXECUTE_MEMBER_TRAIT)

template <typename F, typename... SupportableProperties>
struct execute_member<execution::any_executor<SupportableProperties...>, F>
{
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = false;
  typedef void result_type;
};

#endif // !defined(ASIO_HAS_DEDUCED_EXECUTE_MEMBER_TRAIT)

#if !defined(ASIO_HAS_DEDUCED_QUERY_MEMBER_TRAIT)

template <typename Prop, typename... SupportableProperties>
struct query_member<
    execution::any_executor<SupportableProperties...>, Prop,
    enable_if_t<
      execution::detail::supportable_properties<
        0, void(SupportableProperties...)>::template
          find_convertible_property<Prop>::value
    >>
{
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = false;
  typedef typename execution::detail::supportable_properties<
      0, void(SupportableProperties...)>::template
        find_convertible_property<Prop>::query_result_type result_type;
};

#endif // !defined(ASIO_HAS_DEDUCED_QUERY_MEMBER_TRAIT)

#if !defined(ASIO_HAS_DEDUCED_REQUIRE_MEMBER_TRAIT)

template <typename Prop, typename... SupportableProperties>
struct require_member<
    execution::any_executor<SupportableProperties...>, Prop,
    enable_if_t<
      execution::detail::supportable_properties<
        0, void(SupportableProperties...)>::template
          find_convertible_requirable_property<Prop>::value
    >>
{
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = false;
  typedef execution::any_executor<SupportableProperties...> result_type;
};

#endif // !defined(ASIO_HAS_DEDUCED_REQUIRE_MEMBER_TRAIT)

#if !defined(ASIO_HAS_DEDUCED_PREFER_FREE_TRAIT)

template <typename Prop, typename... SupportableProperties>
struct prefer_member<
    execution::any_executor<SupportableProperties...>, Prop,
    enable_if_t<
      execution::detail::supportable_properties<
        0, void(SupportableProperties...)>::template
          find_convertible_preferable_property<Prop>::value
    >>
{
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = false;
  typedef execution::any_executor<SupportableProperties...> result_type;
};

#endif // !defined(ASIO_HAS_DEDUCED_PREFER_FREE_TRAIT)

} // namespace traits

#endif // defined(GENERATING_DOCUMENTATION)

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_EXECUTION_ANY_EXECUTOR_HPP
