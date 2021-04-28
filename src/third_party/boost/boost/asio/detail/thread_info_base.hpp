//
// detail/thread_info_base.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2021 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_THREAD_INFO_BASE_HPP
#define BOOST_ASIO_DETAIL_THREAD_INFO_BASE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <climits>
#include <cstddef>
#include <boost/asio/detail/noncopyable.hpp>

#if defined(BOOST_ASIO_HAS_STD_EXCEPTION_PTR) \
  && !defined(BOOST_ASIO_NO_EXCEPTIONS)
# include <exception>
# include <boost/asio/multiple_exceptions.hpp>
#endif // defined(BOOST_ASIO_HAS_STD_EXCEPTION_PTR)
       // && !defined(BOOST_ASIO_NO_EXCEPTIONS)

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

class thread_info_base
  : private noncopyable
{
public:
  struct default_tag
  {
    enum { mem_index = 0 };
  };

  struct awaitable_frame_tag
  {
    enum { mem_index = 1 };
  };

  struct executor_function_tag
  {
    enum { mem_index = 2 };
  };

  thread_info_base()
#if defined(BOOST_ASIO_HAS_STD_EXCEPTION_PTR) \
  && !defined(BOOST_ASIO_NO_EXCEPTIONS)
    : has_pending_exception_(0)
#endif // defined(BOOST_ASIO_HAS_STD_EXCEPTION_PTR)
       // && !defined(BOOST_ASIO_NO_EXCEPTIONS)
  {
    for (int i = 0; i < max_mem_index; ++i)
      reusable_memory_[i] = 0;
  }

  ~thread_info_base()
  {
    for (int i = 0; i < max_mem_index; ++i)
    {
      // The following test for non-null pointers is technically redundant, but
      // it is significantly faster when using a tight io_context::poll() loop
      // in latency sensitive applications.
      if (reusable_memory_[i])
        ::operator delete(reusable_memory_[i]);
    }
  }

  static void* allocate(thread_info_base* this_thread, std::size_t size)
  {
    return allocate(default_tag(), this_thread, size);
  }

  static void deallocate(thread_info_base* this_thread,
      void* pointer, std::size_t size)
  {
    deallocate(default_tag(), this_thread, pointer, size);
  }

  template <typename Purpose>
  static void* allocate(Purpose, thread_info_base* this_thread,
      std::size_t size)
  {
    std::size_t chunks = (size + chunk_size - 1) / chunk_size;

    if (this_thread && this_thread->reusable_memory_[Purpose::mem_index])
    {
      void* const pointer = this_thread->reusable_memory_[Purpose::mem_index];
      this_thread->reusable_memory_[Purpose::mem_index] = 0;

      unsigned char* const mem = static_cast<unsigned char*>(pointer);
      if (static_cast<std::size_t>(mem[0]) >= chunks)
      {
        mem[size] = mem[0];
        return pointer;
      }

      ::operator delete(pointer);
    }

    void* const pointer = ::operator new(chunks * chunk_size + 1);
    unsigned char* const mem = static_cast<unsigned char*>(pointer);
    mem[size] = (chunks <= UCHAR_MAX) ? static_cast<unsigned char>(chunks) : 0;
    return pointer;
  }

  template <typename Purpose>
  static void deallocate(Purpose, thread_info_base* this_thread,
      void* pointer, std::size_t size)
  {
    if (size <= chunk_size * UCHAR_MAX)
    {
      if (this_thread && this_thread->reusable_memory_[Purpose::mem_index] == 0)
      {
        unsigned char* const mem = static_cast<unsigned char*>(pointer);
        mem[0] = mem[size];
        this_thread->reusable_memory_[Purpose::mem_index] = pointer;
        return;
      }
    }

    ::operator delete(pointer);
  }

  void capture_current_exception()
  {
#if defined(BOOST_ASIO_HAS_STD_EXCEPTION_PTR) \
  && !defined(BOOST_ASIO_NO_EXCEPTIONS)
    switch (has_pending_exception_)
    {
    case 0:
      has_pending_exception_ = 1;
      pending_exception_ = std::current_exception();
      break;
    case 1:
      has_pending_exception_ = 2;
      pending_exception_ =
        std::make_exception_ptr<multiple_exceptions>(
            multiple_exceptions(pending_exception_));
      break;
    default:
      break;
    }
#endif // defined(BOOST_ASIO_HAS_STD_EXCEPTION_PTR)
       // && !defined(BOOST_ASIO_NO_EXCEPTIONS)
  }

  void rethrow_pending_exception()
  {
#if defined(BOOST_ASIO_HAS_STD_EXCEPTION_PTR) \
  && !defined(BOOST_ASIO_NO_EXCEPTIONS)
    if (has_pending_exception_ > 0)
    {
      has_pending_exception_ = 0;
      std::exception_ptr ex(
          BOOST_ASIO_MOVE_CAST(std::exception_ptr)(
            pending_exception_));
      std::rethrow_exception(ex);
    }
#endif // defined(BOOST_ASIO_HAS_STD_EXCEPTION_PTR)
       // && !defined(BOOST_ASIO_NO_EXCEPTIONS)
  }

private:
  enum { chunk_size = 4 };
  enum { max_mem_index = 3 };
  void* reusable_memory_[max_mem_index];

#if defined(BOOST_ASIO_HAS_STD_EXCEPTION_PTR) \
  && !defined(BOOST_ASIO_NO_EXCEPTIONS)
  int has_pending_exception_;
  std::exception_ptr pending_exception_;
#endif // defined(BOOST_ASIO_HAS_STD_EXCEPTION_PTR)
       // && !defined(BOOST_ASIO_NO_EXCEPTIONS)
};

} // namespace detail
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_DETAIL_THREAD_INFO_BASE_HPP
