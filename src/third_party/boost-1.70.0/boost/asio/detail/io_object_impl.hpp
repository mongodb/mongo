//
// io_object_impl.hpp
// ~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_IO_OBJECT_IMPL_HPP
#define BOOST_ASIO_DETAIL_IO_OBJECT_IMPL_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <new>
#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/io_object_executor.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/asio/io_context.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

class executor;

namespace detail {

inline bool is_native_io_executor(const io_context::executor_type&)
{
  return true;
}

template <typename Executor>
inline bool is_native_io_executor(const Executor&,
    typename enable_if<!is_same<Executor, executor>::value>::type* = 0)
{
  return false;
}

template <typename Executor>
inline bool is_native_io_executor(const Executor& ex,
    typename enable_if<is_same<Executor, executor>::value>::type* = 0)
{
#if !defined (BOOST_ASIO_NO_TYPEID)
  return ex.target_type() == typeid(io_context::executor_type);
#else // !defined (BOOST_ASIO_NO_TYPEID)
  return false;
#endif // !defined (BOOST_ASIO_NO_TYPEID)
}

template <typename IoObjectService,
    typename Executor = io_context::executor_type>
class io_object_impl
{
public:
  // The type of the service that will be used to provide I/O operations.
  typedef IoObjectService service_type;

  // The underlying implementation type of I/O object.
  typedef typename service_type::implementation_type implementation_type;

  // The type of the executor associated with the object.
  typedef Executor executor_type;

  // The type of executor to be used when implementing asynchronous operations.
  typedef io_object_executor<Executor> implementation_executor_type;

  // Construct an I/O object using an executor.
  explicit io_object_impl(const executor_type& ex)
    : service_(&boost::asio::use_service<IoObjectService>(ex.context())),
      implementation_executor_(ex, (is_native_io_executor)(ex))
  {
    service_->construct(implementation_);
  }

  // Construct an I/O object using an execution context.
  template <typename ExecutionContext>
  explicit io_object_impl(ExecutionContext& context,
      typename enable_if<is_convertible<
        ExecutionContext&, execution_context&>::value>::type* = 0)
    : service_(&boost::asio::use_service<IoObjectService>(context)),
      implementation_executor_(context.get_executor(),
        is_same<ExecutionContext, io_context>::value)
  {
    service_->construct(implementation_);
  }

#if defined(BOOST_ASIO_HAS_MOVE)
  // Move-construct an I/O object.
  io_object_impl(io_object_impl&& other)
    : service_(&other.get_service()),
      implementation_executor_(other.get_implementation_executor())
  {
    service_->move_construct(implementation_, other.implementation_);
  }

  // Perform a converting move-construction of an I/O object.
  template <typename IoObjectService1, typename Executor1>
  io_object_impl(io_object_impl<IoObjectService1, Executor1>&& other)
    : service_(&boost::asio::use_service<IoObjectService>(
            other.get_implementation_executor().context())),
      implementation_executor_(other.get_implementation_executor())
  {
    service_->converting_move_construct(implementation_,
        other.get_service(), other.get_implementation());
  }
#endif // defined(BOOST_ASIO_HAS_MOVE)

  // Destructor.
  ~io_object_impl()
  {
    service_->destroy(implementation_);
  }

#if defined(BOOST_ASIO_HAS_MOVE)
  // Move-assign an I/O object.
  io_object_impl& operator=(io_object_impl&& other)
  {
    if (this != &other)
    {
      service_->move_assign(implementation_,
          *other.service_, other.implementation_);
      implementation_executor_.~implementation_executor_type();
      new (&implementation_executor_) implementation_executor_type(
          std::move(other.implementation_executor_));
      service_ = other.service_;
    }
    return *this;
  }
#endif // defined(BOOST_ASIO_HAS_MOVE)

  // Get the executor associated with the object.
  executor_type get_executor() BOOST_ASIO_NOEXCEPT
  {
    return implementation_executor_.inner_executor();
  }

  // Get the executor to be used when implementing asynchronous operations.
  const implementation_executor_type& get_implementation_executor()
    BOOST_ASIO_NOEXCEPT
  {
    return implementation_executor_;
  }

  // Get the service associated with the I/O object.
  service_type& get_service()
  {
    return *service_;
  }

  // Get the service associated with the I/O object.
  const service_type& get_service() const
  {
    return *service_;
  }

  // Get the underlying implementation of the I/O object.
  implementation_type& get_implementation()
  {
    return implementation_;
  }

  // Get the underlying implementation of the I/O object.
  const implementation_type& get_implementation() const
  {
    return implementation_;
  }

private:
  // Disallow copying and copy assignment.
  io_object_impl(const io_object_impl&);
  io_object_impl& operator=(const io_object_impl&);

  // The service associated with the I/O object.
  service_type* service_;

  // The underlying implementation of the I/O object.
  implementation_type implementation_;

  // The associated executor.
  implementation_executor_type implementation_executor_;
};

} // namespace detail
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_DETAIL_IO_OBJECT_IMPL_HPP
