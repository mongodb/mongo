//
// detail/thread_info_base.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_THREAD_INFO_BASE_HPP
#define ASIO_DETAIL_THREAD_INFO_BASE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <climits>
#include <cstddef>
#include "asio/detail/noncopyable.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

class thread_info_base
  : private noncopyable
{
public:
  thread_info_base()
    : reusable_memory_(0)
  {
  }

  ~thread_info_base()
  {
    if (reusable_memory_)
      ::operator delete(reusable_memory_);
  }

  static void* allocate(thread_info_base* this_thread, std::size_t size)
  {
    std::size_t chunks = (size + chunk_size - 1) / chunk_size;

    if (this_thread && this_thread->reusable_memory_)
    {
      void* const pointer = this_thread->reusable_memory_;
      this_thread->reusable_memory_ = 0;

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

  static void deallocate(thread_info_base* this_thread,
      void* pointer, std::size_t size)
  {
    if (size <= chunk_size * UCHAR_MAX)
    {
      if (this_thread && this_thread->reusable_memory_ == 0)
      {
        unsigned char* const mem = static_cast<unsigned char*>(pointer);
        mem[0] = mem[size];
        this_thread->reusable_memory_ = pointer;
        return;
      }
    }

    ::operator delete(pointer);
  }

private:
  enum { chunk_size = 4 };
  void* reusable_memory_;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_THREAD_INFO_BASE_HPP
