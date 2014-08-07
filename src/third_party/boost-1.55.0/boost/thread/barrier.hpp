// Copyright (C) 2002-2003
// David Moore, William E. Kempf
// Copyright (C) 2007-8 Anthony Williams
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_BARRIER_JDM030602_HPP
#define BOOST_BARRIER_JDM030602_HPP

#include <boost/thread/detail/config.hpp>
#include <boost/thread/detail/delete.hpp>

#include <boost/throw_exception.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_types.hpp>
#include <boost/thread/condition_variable.hpp>
#include <string>
#include <stdexcept>
#ifdef BOOST_NO_CXX11_HDR_FUNCTIONAL
#include <boost/function.hpp>
#else
#include <functional>
#endif
#include <boost/type_traits/is_same.hpp>
#include <boost/type_traits/is_void.hpp>
#include <boost/utility/enable_if.hpp>
#include <boost/utility/result_of.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{
  namespace thread_detail
  {
#ifdef BOOST_NO_CXX11_HDR_FUNCTIONAL
    typedef function<void()> void_completion_function;
    typedef function<size_t()> size_completion_function;
#else
    typedef std::function<void()> void_completion_function;
    typedef std::function<size_t()> size_completion_function;
#endif

    struct default_barrier_reseter
    {
      unsigned int size_;
      default_barrier_reseter(unsigned int size) :
        size_(size)
      {
      }
      unsigned int operator()()
      {
        return size_;
      }
    };

    struct void_functor_barrier_reseter
    {
      unsigned int size_;
      void_completion_function fct_;
      template <typename F>
#ifndef BOOST_NO_CXX11_HDR_FUNCTIONAL
      void_functor_barrier_reseter(unsigned int size, BOOST_THREAD_RV_REF(F) funct)
      : size_(size), fct_(boost::move(funct))
      {}
#else
      void_functor_barrier_reseter(unsigned int size, F funct)
      : size_(size), fct_(funct)
      {}
#endif
      unsigned int operator()()
      {
        fct_();
        return size_;
      }
    };
    struct void_fct_ptr_barrier_reseter
    {
      unsigned int size_;
      void(*fct_)();
      void_fct_ptr_barrier_reseter(unsigned int size, void(*funct)()) :
        size_(size), fct_(funct)
      {
      }
      unsigned int operator()()
      {
        fct_();
        return size_;
      }
    };
  }
  class barrier
  {
    static inline unsigned int check_counter(unsigned int count)
    {
      if (count == 0) boost::throw_exception(
          thread_exception(system::errc::invalid_argument, "barrier constructor: count cannot be zero."));
      return count;
    }
    struct dummy
    {
    };

  public:
    BOOST_THREAD_NO_COPYABLE( barrier)

    explicit barrier(unsigned int count) :
      m_count(check_counter(count)), m_generation(0), fct_(thread_detail::default_barrier_reseter(count))
    {
    }

    template <typename F>
    barrier(
        unsigned int count,
#ifndef BOOST_NO_CXX11_HDR_FUNCTIONAL
        BOOST_THREAD_RV_REF(F) funct,
#else
        F funct,
#endif
        typename enable_if<
        typename is_void<typename result_of<F>::type>::type, dummy*
        >::type=0
    )
    : m_count(check_counter(count)),
    m_generation(0),
    fct_(thread_detail::void_functor_barrier_reseter(count,
#ifndef BOOST_NO_CXX11_HDR_FUNCTIONAL
            boost::move(funct)
#else
            funct
#endif
        )
    )
    {
    }

    template <typename F>
    barrier(
        unsigned int count,
#ifndef BOOST_NO_CXX11_HDR_FUNCTIONAL
        BOOST_THREAD_RV_REF(F) funct,
#else
        F funct,
#endif
        typename enable_if<
        typename is_same<typename result_of<F>::type, unsigned int>::type, dummy*
        >::type=0
    )
    : m_count(check_counter(count)),
    m_generation(0),
    fct_(
#ifndef BOOST_NO_CXX11_HDR_FUNCTIONAL
        boost::move(funct)
#else
        funct
#endif
    )
    {
    }

    barrier(unsigned int count, void(*funct)()) :
      m_count(check_counter(count)), m_generation(0),
      fct_(funct
          ? thread_detail::size_completion_function(thread_detail::void_fct_ptr_barrier_reseter(count, funct))
          : thread_detail::size_completion_function(thread_detail::default_barrier_reseter(count))
      )
    {
    }
    barrier(unsigned int count, unsigned int(*funct)()) :
      m_count(check_counter(count)), m_generation(0),
      fct_(funct
          ? thread_detail::size_completion_function(funct)
          : thread_detail::size_completion_function(thread_detail::default_barrier_reseter(count))
      )
    {
    }

    bool wait()
    {
      boost::unique_lock < boost::mutex > lock(m_mutex);
      unsigned int gen = m_generation;

      if (--m_count == 0)
      {
        m_generation++;
        m_count = static_cast<unsigned int>(fct_());
        BOOST_ASSERT(m_count != 0);
        m_cond.notify_all();
        return true;
      }

      while (gen == m_generation)
        m_cond.wait(lock);
      return false;
    }

    void count_down_and_wait()
    {
      wait();
    }

  private:
    mutex m_mutex;
    condition_variable m_cond;
    unsigned int m_count;
    unsigned int m_generation;
    thread_detail::size_completion_function fct_;
  };

} // namespace boost

#include <boost/config/abi_suffix.hpp>

#endif
