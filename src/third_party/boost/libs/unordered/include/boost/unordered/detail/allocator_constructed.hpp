/* Copyright 2024 Braden Ganetsky.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_ALLOCATOR_CONSTRUCTED_HPP
#define BOOST_UNORDERED_DETAIL_ALLOCATOR_CONSTRUCTED_HPP

#include <boost/core/allocator_traits.hpp>
#include <boost/unordered/detail/opt_storage.hpp>

namespace boost {
  namespace unordered {
    namespace detail {

      struct allocator_policy
      {
        template <class Allocator, class T, class... Args>
        static void construct(Allocator& a, T* p, Args&&... args)
        {
          boost::allocator_construct(a, p, std::forward<Args>(args)...);
        }

        template <class Allocator, class T>
        static void destroy(Allocator& a, T* p)
        {
          boost::allocator_destroy(a, p);
        }
      };

      /* constructs a stack-based object with the given policy and allocator */
      template <class Allocator, class T, class Policy = allocator_policy>
      class allocator_constructed
      {
        opt_storage<T> storage;
        Allocator alloc;

      public:
        template <class... Args>
        allocator_constructed(Allocator const& alloc_, Args&&... args)
            : alloc(alloc_)
        {
          Policy::construct(
            alloc, storage.address(), std::forward<Args>(args)...);
        }

        ~allocator_constructed() { Policy::destroy(alloc, storage.address()); }

        T& value() { return *storage.address(); }
      };

    } /* namespace detail */
  }   /* namespace unordered */
} /* namespace boost */

#endif
