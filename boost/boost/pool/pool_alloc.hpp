// Copyright (C) 2000, 2001 Stephen Cleary
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_POOL_ALLOC_HPP
#define BOOST_POOL_ALLOC_HPP

// std::numeric_limits
#include <boost/limits.hpp>
// new, std::bad_alloc
#include <new>

#include <boost/pool/poolfwd.hpp>

// boost::singleton_pool
#include <boost/pool/singleton_pool.hpp>

#include <boost/detail/workaround.hpp>

// The following code will be put into Boost.Config in a later revision
#if defined(_RWSTD_VER) || defined(__SGI_STL_PORT) || \
    BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x582))
 #define BOOST_NO_PROPER_STL_DEALLOCATE
#endif

namespace boost {

struct pool_allocator_tag { };

template <typename T,
    typename UserAllocator,
    typename Mutex,
    unsigned NextSize>
class pool_allocator
{
  public:
    typedef T value_type;
    typedef UserAllocator user_allocator;
    typedef Mutex mutex;
    BOOST_STATIC_CONSTANT(unsigned, next_size = NextSize);

    typedef value_type * pointer;
    typedef const value_type * const_pointer;
    typedef value_type & reference;
    typedef const value_type & const_reference;
    typedef typename pool<UserAllocator>::size_type size_type;
    typedef typename pool<UserAllocator>::difference_type difference_type;

    template <typename U>
    struct rebind
    {
      typedef pool_allocator<U, UserAllocator, Mutex, NextSize> other;
    };

  public:
    pool_allocator() { }

    // default copy constructor

    // default assignment operator

    // not explicit, mimicking std::allocator [20.4.1]
    template <typename U>
    pool_allocator(const pool_allocator<U, UserAllocator, Mutex, NextSize> &)
    { }

    // default destructor

    static pointer address(reference r)
    { return &r; }
    static const_pointer address(const_reference s)
    { return &s; }
    static size_type max_size()
    { return (std::numeric_limits<size_type>::max)(); }
    static void construct(const pointer ptr, const value_type & t)
    { new (ptr) T(t); }
    static void destroy(const pointer ptr)
    {
      ptr->~T();
      (void) ptr; // avoid unused variable warning
    }

    bool operator==(const pool_allocator &) const
    { return true; }
    bool operator!=(const pool_allocator &) const
    { return false; }

    static pointer allocate(const size_type n)
    {
      const pointer ret = static_cast<pointer>(
          singleton_pool<pool_allocator_tag, sizeof(T), UserAllocator, Mutex,
              NextSize>::ordered_malloc(n) );
      if (ret == 0)
        throw std::bad_alloc();
      return ret;
    }
    static pointer allocate(const size_type n, const void * const)
    { return allocate(n); }
    static void deallocate(const pointer ptr, const size_type n)
    {
#ifdef BOOST_NO_PROPER_STL_DEALLOCATE
      if (ptr == 0 || n == 0)
        return;
#endif
      singleton_pool<pool_allocator_tag, sizeof(T), UserAllocator, Mutex,
          NextSize>::ordered_free(ptr, n);
    }
};

struct fast_pool_allocator_tag { };

template <typename T,
    typename UserAllocator,
    typename Mutex,
    unsigned NextSize>
class fast_pool_allocator
{
  public:
    typedef T value_type;
    typedef UserAllocator user_allocator;
    typedef Mutex mutex;
    BOOST_STATIC_CONSTANT(unsigned, next_size = NextSize);

    typedef value_type * pointer;
    typedef const value_type * const_pointer;
    typedef value_type & reference;
    typedef const value_type & const_reference;
    typedef typename pool<UserAllocator>::size_type size_type;
    typedef typename pool<UserAllocator>::difference_type difference_type;

    template <typename U>
    struct rebind
    {
      typedef fast_pool_allocator<U, UserAllocator, Mutex, NextSize> other;
    };

  public:
    fast_pool_allocator() { }

    // default copy constructor

    // default assignment operator

    // not explicit, mimicking std::allocator [20.4.1]
    template <typename U>
    fast_pool_allocator(
        const fast_pool_allocator<U, UserAllocator, Mutex, NextSize> &)
    { }

    // default destructor

    static pointer address(reference r)
    { return &r; }
    static const_pointer address(const_reference s)
    { return &s; }
    static size_type max_size()
    { return (std::numeric_limits<size_type>::max)(); }
    void construct(const pointer ptr, const value_type & t)
    { new (ptr) T(t); }
    void destroy(const pointer ptr)
    {
      ptr->~T();
      (void) ptr; // avoid unused variable warning
    }

    bool operator==(const fast_pool_allocator &) const
    { return true; }
    bool operator!=(const fast_pool_allocator &) const
    { return false; }

    static pointer allocate(const size_type n)
    {
      const pointer ret = (n == 1) ? 
          static_cast<pointer>(
              singleton_pool<fast_pool_allocator_tag, sizeof(T),
                  UserAllocator, Mutex, NextSize>::malloc() ) :
          static_cast<pointer>(
              singleton_pool<fast_pool_allocator_tag, sizeof(T),
                  UserAllocator, Mutex, NextSize>::ordered_malloc(n) );
      if (ret == 0)
        throw std::bad_alloc();
      return ret;
    }
    static pointer allocate(const size_type n, const void * const)
    { return allocate(n); }
    static pointer allocate()
    {
      const pointer ret = static_cast<pointer>(
          singleton_pool<fast_pool_allocator_tag, sizeof(T),
              UserAllocator, Mutex, NextSize>::malloc() );
      if (ret == 0)
        throw std::bad_alloc();
      return ret;
    }
    static void deallocate(const pointer ptr, const size_type n)
    {
#ifdef BOOST_NO_PROPER_STL_DEALLOCATE
      if (ptr == 0 || n == 0)
        return;
#endif
      if (n == 1)
        singleton_pool<fast_pool_allocator_tag, sizeof(T),
            UserAllocator, Mutex, NextSize>::free(ptr);
      else
        singleton_pool<fast_pool_allocator_tag, sizeof(T),
            UserAllocator, Mutex, NextSize>::free(ptr, n);
    }
    static void deallocate(const pointer ptr)
    {
      singleton_pool<fast_pool_allocator_tag, sizeof(T),
          UserAllocator, Mutex, NextSize>::free(ptr);
    }
};

} // namespace boost

#endif
