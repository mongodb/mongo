// Copyright (C) 2000, 2001 Stephen Cleary
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_SINGLETON_POOL_HPP
#define BOOST_SINGLETON_POOL_HPP

#include <boost/pool/poolfwd.hpp>

// boost::pool
#include <boost/pool/pool.hpp>
// boost::details::pool::singleton_default
#include <boost/pool/detail/singleton.hpp>
// boost::details::pool::guard
#include <boost/pool/detail/guard.hpp>

namespace boost {

//
// The singleton_pool class allows other pool interfaces for types of the same
//   size to share the same pool
//
template <typename Tag, unsigned RequestedSize,
    typename UserAllocator,
    typename Mutex,
    unsigned NextSize>
struct singleton_pool
{
  public:
    typedef Tag tag;
    typedef Mutex mutex;
    typedef UserAllocator user_allocator;
    typedef typename pool<UserAllocator>::size_type size_type;
    typedef typename pool<UserAllocator>::difference_type difference_type;

    BOOST_STATIC_CONSTANT(unsigned, requested_size = RequestedSize);
    BOOST_STATIC_CONSTANT(unsigned, next_size = NextSize);

  private:
    struct pool_type: Mutex
    {
      pool<UserAllocator> p;
      pool_type():p(RequestedSize, NextSize) { }
    };

    typedef details::pool::singleton_default<pool_type> singleton;

    singleton_pool();

  public:
    static void * malloc()
    {
      pool_type & p = singleton::instance();
      details::pool::guard<Mutex> g(p);
      return p.p.malloc();
    }
    static void * ordered_malloc()
    {
      pool_type & p = singleton::instance();
      details::pool::guard<Mutex> g(p);
      return p.p.ordered_malloc();
    }
    static void * ordered_malloc(const size_type n)
    {
      pool_type & p = singleton::instance();
      details::pool::guard<Mutex> g(p);
      return p.p.ordered_malloc(n);
    }
    static bool is_from(void * const ptr)
    {
      pool_type & p = singleton::instance();
      details::pool::guard<Mutex> g(p);
      return p.p.is_from(ptr);
    }
    static void free(void * const ptr)
    {
      pool_type & p = singleton::instance();
      details::pool::guard<Mutex> g(p);
      p.p.free(ptr);
    }
    static void ordered_free(void * const ptr)
    {
      pool_type & p = singleton::instance();
      details::pool::guard<Mutex> g(p);
      p.p.ordered_free(ptr);
    }
    static void free(void * const ptr, const size_type n)
    {
      pool_type & p = singleton::instance();
      details::pool::guard<Mutex> g(p);
      p.p.free(ptr, n);
    }
    static void ordered_free(void * const ptr, const size_type n)
    {
      pool_type & p = singleton::instance();
      details::pool::guard<Mutex> g(p);
      p.p.ordered_free(ptr, n);
    }
    static bool release_memory()
    {
      pool_type & p = singleton::instance();
      details::pool::guard<Mutex> g(p);
      return p.p.release_memory();
    }
    static bool purge_memory()
    {
      pool_type & p = singleton::instance();
      details::pool::guard<Mutex> g(p);
      return p.p.purge_memory();
    }
};

} // namespace boost

#endif
