// Copyright (C) 2023 Christian Mazakas
// Copyright (C) 2024 Braden Ganetsky
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_FOA_NODE_MAP_TYPES_HPP
#define BOOST_UNORDERED_DETAIL_FOA_NODE_MAP_TYPES_HPP

#include <boost/unordered/detail/foa/element_type.hpp>
#include <boost/unordered/detail/foa/types_constructibility.hpp>
#include <boost/unordered/detail/type_traits.hpp>

#include <boost/core/allocator_access.hpp>
#include <boost/core/no_exceptions_support.hpp>
#include <boost/core/pointer_traits.hpp>

namespace boost {
  namespace unordered {
    namespace detail {
      namespace foa {
        template <class Key, class T, class VoidPtr> struct node_map_types
        {
          using key_type = Key;
          using mapped_type = T;
          using raw_key_type = typename std::remove_const<Key>::type;
          using raw_mapped_type = typename std::remove_const<T>::type;

          using init_type = std::pair<raw_key_type, raw_mapped_type>;
          using value_type = std::pair<Key const, T>;
          using moved_type = std::pair<raw_key_type&&, raw_mapped_type&&>;

          using element_type = foa::element_type<value_type, VoidPtr>;

          using types = node_map_types<Key, T, VoidPtr>;
          using constructibility_checker = map_types_constructibility<types>;

          static value_type& value_from(element_type const& x)
          {
            return *(x.p);
          }

          template <class K, class V>
          static raw_key_type const& extract(std::pair<K, V> const& kv)
          {
            return kv.first;
          }

          static raw_key_type const& extract(element_type const& kv)
          {
            return kv.p->first;
          }

          static element_type&& move(element_type& x) { return std::move(x); }
          static moved_type move(init_type& x)
          {
            return {std::move(x.first), std::move(x.second)};
          }

          static moved_type move(value_type& x)
          {
            return {std::move(const_cast<raw_key_type&>(x.first)),
              std::move(const_cast<raw_mapped_type&>(x.second))};
          }

          template <class A>
          static void construct(A&, element_type* p, element_type&& x) noexcept
          {
            p->p = x.p;
            x.p = nullptr;
          }

          template <class A>
          static void construct(
            A& al, element_type* p, element_type const& copy)
          {
            construct(al, p, detail::as_const(*copy.p));
          }

          template <class A, class... Args>
          static void construct(A& al, init_type* p, Args&&... args)
          {
            constructibility_checker::check(al, p, std::forward<Args>(args)...);
            boost::allocator_construct(al, p, std::forward<Args>(args)...);
          }

          template <class A, class... Args>
          static void construct(A& al, value_type* p, Args&&... args)
          {
            constructibility_checker::check(al, p, std::forward<Args>(args)...);
            boost::allocator_construct(al, p, std::forward<Args>(args)...);
          }

          template <class A, class... Args>
          static void construct(A& al, key_type* p, Args&&... args)
          {
            constructibility_checker::check(al, p, std::forward<Args>(args)...);
            boost::allocator_construct(al, p, std::forward<Args>(args)...);
          }

          template <class A, class... Args>
          static void construct(A& al, element_type* p, Args&&... args)
          {
            p->p = boost::allocator_allocate(al, 1);
            BOOST_TRY
            {
              auto address = boost::to_address(p->p);
              constructibility_checker::check(
                al, address, std::forward<Args>(args)...);
              boost::allocator_construct(
                al, address, std::forward<Args>(args)...);
            }
            BOOST_CATCH(...)
            {
              boost::allocator_deallocate(al, p->p, 1);
              BOOST_RETHROW
            }
            BOOST_CATCH_END
          }

          template <class A> static void destroy(A& al, value_type* p) noexcept
          {
            boost::allocator_destroy(al, p);
          }

          template <class A> static void destroy(A& al, init_type* p) noexcept
          {
            boost::allocator_destroy(al, p);
          }

          template <class A> static void destroy(A& al, key_type* p) noexcept
          {
            boost::allocator_destroy(al, p);
          }

          template <class A>
          static void destroy(A& al, element_type* p) noexcept
          {
            if (p->p) {
              destroy(al, boost::to_address(p->p));
              boost::allocator_deallocate(al, p->p, 1);
            }
          }
        };

      } // namespace foa
    } // namespace detail
  } // namespace unordered
} // namespace boost

#endif // BOOST_UNORDERED_DETAIL_FOA_NODE_MAP_TYPES_HPP
