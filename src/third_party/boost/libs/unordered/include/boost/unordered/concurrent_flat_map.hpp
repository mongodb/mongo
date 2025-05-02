/* Fast open-addressing concurrent hashmap.
 *
 * Copyright 2023 Christian Mazakas.
 * Copyright 2023-2024 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_CONCURRENT_FLAT_MAP_HPP
#define BOOST_UNORDERED_CONCURRENT_FLAT_MAP_HPP

#include <boost/unordered/concurrent_flat_map_fwd.hpp>
#include <boost/unordered/detail/concurrent_static_asserts.hpp>
#include <boost/unordered/detail/foa/concurrent_table.hpp>
#include <boost/unordered/detail/foa/flat_map_types.hpp>
#include <boost/unordered/detail/type_traits.hpp>
#include <boost/unordered/unordered_flat_map_fwd.hpp>

#include <boost/container_hash/hash.hpp>
#include <boost/core/allocator_access.hpp>
#include <boost/core/serialization.hpp>

#include <type_traits>

namespace boost {
  namespace unordered {
    template <class Key, class T, class Hash, class Pred, class Allocator>
    class concurrent_flat_map
    {
    private:
      template <class Key2, class T2, class Hash2, class Pred2,
        class Allocator2>
      friend class concurrent_flat_map;
      template <class Key2, class T2, class Hash2, class Pred2,
        class Allocator2>
      friend class unordered_flat_map;

      using type_policy = detail::foa::flat_map_types<Key, T>;

      using table_type =
        detail::foa::concurrent_table<type_policy, Hash, Pred, Allocator>;

      table_type table_;

      template <class K, class V, class H, class KE, class A>
      bool friend operator==(concurrent_flat_map<K, V, H, KE, A> const& lhs,
        concurrent_flat_map<K, V, H, KE, A> const& rhs);

      template <class K, class V, class H, class KE, class A, class Predicate>
      friend typename concurrent_flat_map<K, V, H, KE, A>::size_type erase_if(
        concurrent_flat_map<K, V, H, KE, A>& set, Predicate pred);

      template<class Archive, class K, class V, class H, class KE, class A>
      friend void serialize(
        Archive& ar, concurrent_flat_map<K, V, H, KE, A>& c,
        unsigned int version);

    public:
      using key_type = Key;
      using mapped_type = T;
      using value_type = typename type_policy::value_type;
      using init_type = typename type_policy::init_type;
      using size_type = std::size_t;
      using difference_type = std::ptrdiff_t;
      using hasher = typename boost::unordered::detail::type_identity<Hash>::type;
      using key_equal = typename boost::unordered::detail::type_identity<Pred>::type;
      using allocator_type = typename boost::unordered::detail::type_identity<Allocator>::type;
      using reference = value_type&;
      using const_reference = value_type const&;
      using pointer = typename boost::allocator_pointer<allocator_type>::type;
      using const_pointer =
        typename boost::allocator_const_pointer<allocator_type>::type;
      static constexpr size_type bulk_visit_size = table_type::bulk_visit_size;

#if defined(BOOST_UNORDERED_ENABLE_STATS)
      using stats = typename table_type::stats;
#endif

      concurrent_flat_map()
          : concurrent_flat_map(detail::foa::default_bucket_count)
      {
      }

      explicit concurrent_flat_map(size_type n, const hasher& hf = hasher(),
        const key_equal& eql = key_equal(),
        const allocator_type& a = allocator_type())
          : table_(n, hf, eql, a)
      {
      }

      template <class InputIterator>
      concurrent_flat_map(InputIterator f, InputIterator l,
        size_type n = detail::foa::default_bucket_count,
        const hasher& hf = hasher(), const key_equal& eql = key_equal(),
        const allocator_type& a = allocator_type())
          : table_(n, hf, eql, a)
      {
        this->insert(f, l);
      }

      concurrent_flat_map(concurrent_flat_map const& rhs)
          : table_(rhs.table_,
              boost::allocator_select_on_container_copy_construction(
                rhs.get_allocator()))
      {
      }

      concurrent_flat_map(concurrent_flat_map&& rhs)
          : table_(std::move(rhs.table_))
      {
      }

      template <class InputIterator>
      concurrent_flat_map(
        InputIterator f, InputIterator l, allocator_type const& a)
          : concurrent_flat_map(f, l, 0, hasher(), key_equal(), a)
      {
      }

      explicit concurrent_flat_map(allocator_type const& a)
          : table_(detail::foa::default_bucket_count, hasher(), key_equal(), a)
      {
      }

      concurrent_flat_map(
        concurrent_flat_map const& rhs, allocator_type const& a)
          : table_(rhs.table_, a)
      {
      }

      concurrent_flat_map(concurrent_flat_map&& rhs, allocator_type const& a)
          : table_(std::move(rhs.table_), a)
      {
      }

      concurrent_flat_map(std::initializer_list<value_type> il,
        size_type n = detail::foa::default_bucket_count,
        const hasher& hf = hasher(), const key_equal& eql = key_equal(),
        const allocator_type& a = allocator_type())
          : concurrent_flat_map(n, hf, eql, a)
      {
        this->insert(il.begin(), il.end());
      }

      concurrent_flat_map(size_type n, const allocator_type& a)
          : concurrent_flat_map(n, hasher(), key_equal(), a)
      {
      }

      concurrent_flat_map(
        size_type n, const hasher& hf, const allocator_type& a)
          : concurrent_flat_map(n, hf, key_equal(), a)
      {
      }

      template <typename InputIterator>
      concurrent_flat_map(
        InputIterator f, InputIterator l, size_type n, const allocator_type& a)
          : concurrent_flat_map(f, l, n, hasher(), key_equal(), a)
      {
      }

      template <typename InputIterator>
      concurrent_flat_map(InputIterator f, InputIterator l, size_type n,
        const hasher& hf, const allocator_type& a)
          : concurrent_flat_map(f, l, n, hf, key_equal(), a)
      {
      }

      concurrent_flat_map(
        std::initializer_list<value_type> il, const allocator_type& a)
          : concurrent_flat_map(
              il, detail::foa::default_bucket_count, hasher(), key_equal(), a)
      {
      }

      concurrent_flat_map(std::initializer_list<value_type> il, size_type n,
        const allocator_type& a)
          : concurrent_flat_map(il, n, hasher(), key_equal(), a)
      {
      }

      concurrent_flat_map(std::initializer_list<value_type> il, size_type n,
        const hasher& hf, const allocator_type& a)
          : concurrent_flat_map(il, n, hf, key_equal(), a)
      {
      }


      template <bool avoid_explicit_instantiation = true>
      concurrent_flat_map(
        unordered_flat_map<Key, T, Hash, Pred, Allocator>&& other)
          : table_(std::move(other.table_))
      {
      }

      ~concurrent_flat_map() = default;

      concurrent_flat_map& operator=(concurrent_flat_map const& rhs)
      {
        table_ = rhs.table_;
        return *this;
      }

      concurrent_flat_map& operator=(concurrent_flat_map&& rhs) noexcept(
        noexcept(std::declval<table_type&>() = std::declval<table_type&&>()))
      {
        table_ = std::move(rhs.table_);
        return *this;
      }

      concurrent_flat_map& operator=(std::initializer_list<value_type> ilist)
      {
        table_ = ilist;
        return *this;
      }

      /// Capacity
      ///

      size_type size() const noexcept { return table_.size(); }
      size_type max_size() const noexcept { return table_.max_size(); }

      BOOST_ATTRIBUTE_NODISCARD bool empty() const noexcept
      {
        return size() == 0;
      }

      template <class F>
      BOOST_FORCEINLINE size_type visit(key_type const& k, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        return table_.visit(k, f);
      }

      template <class F>
      BOOST_FORCEINLINE size_type visit(key_type const& k, F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.visit(k, f);
      }

      template <class F>
      BOOST_FORCEINLINE size_type cvisit(key_type const& k, F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.visit(k, f);
      }

      template <class K, class F>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value, size_type>::type
      visit(K&& k, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        return table_.visit(std::forward<K>(k), f);
      }

      template <class K, class F>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value, size_type>::type
      visit(K&& k, F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.visit(std::forward<K>(k), f);
      }

      template <class K, class F>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value, size_type>::type
      cvisit(K&& k, F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.visit(std::forward<K>(k), f);
      }

      template<class FwdIterator, class F>
      BOOST_FORCEINLINE
      size_t visit(FwdIterator first, FwdIterator last, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_BULK_VISIT_ITERATOR(FwdIterator)
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        return table_.visit(first, last, f);
      }

      template<class FwdIterator, class F>
      BOOST_FORCEINLINE
      size_t visit(FwdIterator first, FwdIterator last, F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_BULK_VISIT_ITERATOR(FwdIterator)
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.visit(first, last, f);
      }

      template<class FwdIterator, class F>
      BOOST_FORCEINLINE
      size_t cvisit(FwdIterator first, FwdIterator last, F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_BULK_VISIT_ITERATOR(FwdIterator)
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.visit(first, last, f);
      }

      template <class F> size_type visit_all(F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        return table_.visit_all(f);
      }

      template <class F> size_type visit_all(F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.visit_all(f);
      }

      template <class F> size_type cvisit_all(F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.cvisit_all(f);
      }

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)
      template <class ExecPolicy, class F>
      typename std::enable_if<detail::is_execution_policy<ExecPolicy>::value,
        void>::type
      visit_all(ExecPolicy&& p, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        BOOST_UNORDERED_STATIC_ASSERT_EXEC_POLICY(ExecPolicy)
        table_.visit_all(p, f);
      }

      template <class ExecPolicy, class F>
      typename std::enable_if<detail::is_execution_policy<ExecPolicy>::value,
        void>::type
      visit_all(ExecPolicy&& p, F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        BOOST_UNORDERED_STATIC_ASSERT_EXEC_POLICY(ExecPolicy)
        table_.visit_all(p, f);
      }

      template <class ExecPolicy, class F>
      typename std::enable_if<detail::is_execution_policy<ExecPolicy>::value,
        void>::type
      cvisit_all(ExecPolicy&& p, F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        BOOST_UNORDERED_STATIC_ASSERT_EXEC_POLICY(ExecPolicy)
        table_.cvisit_all(p, f);
      }
#endif

      template <class F> bool visit_while(F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        return table_.visit_while(f);
      }

      template <class F> bool visit_while(F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.visit_while(f);
      }

      template <class F> bool cvisit_while(F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.cvisit_while(f);
      }

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)
      template <class ExecPolicy, class F>
      typename std::enable_if<detail::is_execution_policy<ExecPolicy>::value,
        bool>::type
      visit_while(ExecPolicy&& p, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        BOOST_UNORDERED_STATIC_ASSERT_EXEC_POLICY(ExecPolicy)
        return table_.visit_while(p, f);
      }

      template <class ExecPolicy, class F>
      typename std::enable_if<detail::is_execution_policy<ExecPolicy>::value,
        bool>::type
      visit_while(ExecPolicy&& p, F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        BOOST_UNORDERED_STATIC_ASSERT_EXEC_POLICY(ExecPolicy)
        return table_.visit_while(p, f);
      }

      template <class ExecPolicy, class F>
      typename std::enable_if<detail::is_execution_policy<ExecPolicy>::value,
        bool>::type
      cvisit_while(ExecPolicy&& p, F f) const
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        BOOST_UNORDERED_STATIC_ASSERT_EXEC_POLICY(ExecPolicy)
        return table_.cvisit_while(p, f);
      }
#endif

      /// Modifiers
      ///

      template <class Ty>
      BOOST_FORCEINLINE auto insert(Ty&& value)
        -> decltype(table_.insert(std::forward<Ty>(value)))
      {
        return table_.insert(std::forward<Ty>(value));
      }

      BOOST_FORCEINLINE bool insert(init_type&& obj)
      {
        return table_.insert(std::move(obj));
      }

      template <class InputIterator>
      size_type insert(InputIterator begin, InputIterator end)
      {
        size_type count_elements = 0;
        for (auto pos = begin; pos != end; ++pos, ++count_elements) {
          table_.emplace(*pos);
        }
        return count_elements;
      }

      size_type insert(std::initializer_list<value_type> ilist)
      {
        return this->insert(ilist.begin(), ilist.end());
      }

      template <class M>
      BOOST_FORCEINLINE bool insert_or_assign(key_type const& k, M&& obj)
      {
        return table_.try_emplace_or_visit(k, std::forward<M>(obj),
          [&](value_type& m) { m.second = std::forward<M>(obj); });
      }

      template <class M>
      BOOST_FORCEINLINE bool insert_or_assign(key_type&& k, M&& obj)
      {
        return table_.try_emplace_or_visit(std::move(k), std::forward<M>(obj),
          [&](value_type& m) { m.second = std::forward<M>(obj); });
      }

      template <class K, class M>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value, bool>::type
      insert_or_assign(K&& k, M&& obj)
      {
        return table_.try_emplace_or_visit(std::forward<K>(k),
          std::forward<M>(obj),
          [&](value_type& m) { m.second = std::forward<M>(obj); });
      }

      template <class Ty, class F>
      BOOST_FORCEINLINE auto insert_or_visit(Ty&& value, F f)
        -> decltype(table_.insert_or_visit(std::forward<Ty>(value), f))
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        return table_.insert_or_visit(std::forward<Ty>(value), f);
      }

      template <class F>
      BOOST_FORCEINLINE bool insert_or_visit(init_type&& obj, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        return table_.insert_or_visit(std::move(obj), f);
      }

      template <class InputIterator, class F>
      size_type insert_or_visit(InputIterator first, InputIterator last, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        size_type count_elements = 0;
        for (; first != last; ++first, ++count_elements) {
          table_.emplace_or_visit(*first, f);
        }
        return count_elements;
      }

      template <class F>
      size_type insert_or_visit(std::initializer_list<value_type> ilist, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)
        return this->insert_or_visit(ilist.begin(), ilist.end(), std::ref(f));
      }

      template <class Ty, class F>
      BOOST_FORCEINLINE auto insert_or_cvisit(Ty&& value, F f)
        -> decltype(table_.insert_or_cvisit(std::forward<Ty>(value), f))
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.insert_or_cvisit(std::forward<Ty>(value), f);
      }

      template <class F>
      BOOST_FORCEINLINE bool insert_or_cvisit(init_type&& obj, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return table_.insert_or_cvisit(std::move(obj), f);
      }

      template <class InputIterator, class F>
      size_type insert_or_cvisit(InputIterator first, InputIterator last, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        size_type count_elements = 0;
        for (; first != last; ++first, ++count_elements) {
          table_.emplace_or_cvisit(*first, f);
        }
        return count_elements;
      }

      template <class F>
      size_type insert_or_cvisit(std::initializer_list<value_type> ilist, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)
        return this->insert_or_cvisit(ilist.begin(), ilist.end(), std::ref(f));
      }

      template <class Ty, class F1, class F2>
      BOOST_FORCEINLINE auto insert_and_visit(Ty&& value, F1 f1, F2 f2)
        -> decltype(table_.insert_and_visit(std::forward<Ty>(value), f1, f2))
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F1)
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F2)
        return table_.insert_and_visit(std::forward<Ty>(value), f1, f2);
      }

      template <class F1, class F2>
      BOOST_FORCEINLINE bool insert_and_visit(init_type&& obj, F1 f1, F2 f2)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F1)
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F2)
        return table_.insert_and_visit(std::move(obj), f1, f2);
      }

      template <class InputIterator, class F1, class F2>
      size_type insert_and_visit(
        InputIterator first, InputIterator last, F1 f1, F2 f2)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F1)
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F2)
        size_type count_elements = 0;
        for (; first != last; ++first, ++count_elements) {
          table_.emplace_and_visit(*first, f1, f2);
        }
        return count_elements;
      }

      template <class F1, class F2>
      size_type insert_and_visit(
        std::initializer_list<value_type> ilist, F1 f1, F2 f2)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F1)
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F2)
        return this->insert_and_visit(
          ilist.begin(), ilist.end(), std::ref(f1), std::ref(f2));
      }

      template <class Ty, class F1, class F2>
      BOOST_FORCEINLINE auto insert_and_cvisit(Ty&& value, F1 f1, F2 f2)
        -> decltype(table_.insert_and_cvisit(std::forward<Ty>(value), f1, f2))
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F1)
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F2)
        return table_.insert_and_cvisit(std::forward<Ty>(value), f1, f2);
      }

      template <class F1, class F2>
      BOOST_FORCEINLINE bool insert_and_cvisit(init_type&& obj, F1 f1, F2 f2)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F1)
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F2)
        return table_.insert_and_cvisit(std::move(obj), f1, f2);
      }

      template <class InputIterator, class F1, class F2>
      size_type insert_and_cvisit(
        InputIterator first, InputIterator last, F1 f1, F2 f2)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F1)
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F2)
        size_type count_elements = 0;
        for (; first != last; ++first, ++count_elements) {
          table_.emplace_and_cvisit(*first, f1, f2);
        }
        return count_elements;
      }

      template <class F1, class F2>
      size_type insert_and_cvisit(
        std::initializer_list<value_type> ilist, F1 f1, F2 f2)
      {
        BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F1)
        BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F2)
        return this->insert_and_cvisit(
          ilist.begin(), ilist.end(), std::ref(f1), std::ref(f2));
      }

      template <class... Args> BOOST_FORCEINLINE bool emplace(Args&&... args)
      {
        return table_.emplace(std::forward<Args>(args)...);
      }

      template <class Arg, class... Args>
      BOOST_FORCEINLINE bool emplace_or_visit(Arg&& arg, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_INVOCABLE(Arg, Args...)
        return table_.emplace_or_visit(
          std::forward<Arg>(arg), std::forward<Args>(args)...);
      }

      template <class Arg, class... Args>
      BOOST_FORCEINLINE bool emplace_or_cvisit(Arg&& arg, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_CONST_INVOCABLE(Arg, Args...)
        return table_.emplace_or_cvisit(
          std::forward<Arg>(arg), std::forward<Args>(args)...);
      }

      template <class Arg1, class Arg2, class... Args>
      BOOST_FORCEINLINE bool emplace_and_visit(
         Arg1&& arg1, Arg2&& arg2, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_PENULTIMATE_ARG_INVOCABLE(
          Arg1, Arg2, Args...)
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_INVOCABLE(Arg2, Args...)
        return table_.emplace_and_visit(
          std::forward<Arg1>(arg1), std::forward<Arg2>(arg2),
          std::forward<Args>(args)...);
      }

      template <class Arg1, class Arg2, class... Args>
      BOOST_FORCEINLINE bool emplace_and_cvisit(
        Arg1&& arg1, Arg2&& arg2, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_PENULTIMATE_ARG_INVOCABLE(
          Arg1, Arg2, Args...)
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_CONST_INVOCABLE(Arg2, Args...)
        return table_.emplace_and_cvisit(
          std::forward<Arg1>(arg1), std::forward<Arg2>(arg2),
          std::forward<Args>(args)...);
      }

      template <class... Args>
      BOOST_FORCEINLINE bool try_emplace(key_type const& k, Args&&... args)
      {
        return table_.try_emplace(k, std::forward<Args>(args)...);
      }

      template <class... Args>
      BOOST_FORCEINLINE bool try_emplace(key_type&& k, Args&&... args)
      {
        return table_.try_emplace(std::move(k), std::forward<Args>(args)...);
      }

      template <class K, class... Args>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value, bool>::type
      try_emplace(K&& k, Args&&... args)
      {
        return table_.try_emplace(
          std::forward<K>(k), std::forward<Args>(args)...);
      }

      template <class Arg, class... Args>
      BOOST_FORCEINLINE bool try_emplace_or_visit(
        key_type const& k, Arg&& arg, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_INVOCABLE(Arg, Args...)
        return table_.try_emplace_or_visit(
          k, std::forward<Arg>(arg), std::forward<Args>(args)...);
      }

      template <class Arg, class... Args>
      BOOST_FORCEINLINE bool try_emplace_or_cvisit(
        key_type const& k, Arg&& arg, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_CONST_INVOCABLE(Arg, Args...)
        return table_.try_emplace_or_cvisit(
          k, std::forward<Arg>(arg), std::forward<Args>(args)...);
      }

      template <class Arg, class... Args>
      BOOST_FORCEINLINE bool try_emplace_or_visit(
        key_type&& k, Arg&& arg, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_INVOCABLE(Arg, Args...)
        return table_.try_emplace_or_visit(
          std::move(k), std::forward<Arg>(arg), std::forward<Args>(args)...);
      }

      template <class Arg, class... Args>
      BOOST_FORCEINLINE bool try_emplace_or_cvisit(
        key_type&& k, Arg&& arg, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_CONST_INVOCABLE(Arg, Args...)
        return table_.try_emplace_or_cvisit(
          std::move(k), std::forward<Arg>(arg), std::forward<Args>(args)...);
      }

      template <class K, class Arg, class... Args>
      BOOST_FORCEINLINE bool try_emplace_or_visit(
        K&& k, Arg&& arg, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_INVOCABLE(Arg, Args...)
        return table_.try_emplace_or_visit(std::forward<K>(k),
          std::forward<Arg>(arg), std::forward<Args>(args)...);
      }

      template <class K, class Arg, class... Args>
      BOOST_FORCEINLINE bool try_emplace_or_cvisit(
        K&& k, Arg&& arg, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_CONST_INVOCABLE(Arg, Args...)
        return table_.try_emplace_or_cvisit(std::forward<K>(k),
          std::forward<Arg>(arg), std::forward<Args>(args)...);
      }

      template <class Arg1, class Arg2, class... Args>
      BOOST_FORCEINLINE bool try_emplace_and_visit(
        key_type const& k, Arg1&& arg1, Arg2&& arg2, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_PENULTIMATE_ARG_INVOCABLE(
          Arg1, Arg2, Args...)
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_INVOCABLE(Arg2, Args...)
        return table_.try_emplace_and_visit(
          k, std::forward<Arg1>(arg1), std::forward<Arg2>(arg2),
          std::forward<Args>(args)...);
      }

      template <class Arg1, class Arg2, class... Args>
      BOOST_FORCEINLINE bool try_emplace_and_cvisit(
        key_type const& k, Arg1&& arg1, Arg2&& arg2, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_PENULTIMATE_ARG_INVOCABLE(
          Arg1, Arg2, Args...)
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_CONST_INVOCABLE(Arg2, Args...)
        return table_.try_emplace_and_cvisit(
          k, std::forward<Arg1>(arg1), std::forward<Arg2>(arg2),
          std::forward<Args>(args)...);
      }

      template <class Arg1, class Arg2, class... Args>
      BOOST_FORCEINLINE bool try_emplace_and_visit(
        key_type&& k, Arg1&& arg1, Arg2&& arg2, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_PENULTIMATE_ARG_INVOCABLE(
          Arg1, Arg2, Args...)
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_INVOCABLE(Arg2, Args...)
        return table_.try_emplace_and_visit(
          std::move(k), std::forward<Arg1>(arg1), std::forward<Arg2>(arg2),
          std::forward<Args>(args)...);
      }

      template <class Arg1, class Arg2, class... Args>
      BOOST_FORCEINLINE bool try_emplace_and_cvisit(
        key_type&& k, Arg1&& arg1, Arg2&& arg2, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_PENULTIMATE_ARG_INVOCABLE(
          Arg1, Arg2, Args...)
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_CONST_INVOCABLE(Arg2, Args...)
        return table_.try_emplace_and_cvisit(
          std::move(k), std::forward<Arg1>(arg1), std::forward<Arg2>(arg2),
          std::forward<Args>(args)...);
      }

      template <class K, class Arg1, class Arg2, class... Args>
      BOOST_FORCEINLINE bool try_emplace_and_visit(
        K&& k, Arg1&& arg1, Arg2&& arg2, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_PENULTIMATE_ARG_INVOCABLE(
          Arg1, Arg2, Args...)
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_INVOCABLE(Arg2, Args...)
        return table_.try_emplace_and_visit(std::forward<K>(k),
          std::forward<Arg1>(arg1), std::forward<Arg2>(arg2),
          std::forward<Args>(args)...);
      }

      template <class K, class Arg1, class Arg2, class... Args>
      BOOST_FORCEINLINE bool try_emplace_and_cvisit(
        K&& k, Arg1&& arg1, Arg2&& arg2, Args&&... args)
      {
        BOOST_UNORDERED_STATIC_ASSERT_PENULTIMATE_ARG_INVOCABLE(
          Arg1, Arg2, Args...)
        BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_CONST_INVOCABLE(Arg2, Args...)
        return table_.try_emplace_and_cvisit(std::forward<K>(k),
          std::forward<Arg1>(arg1), std::forward<Arg2>(arg2),
          std::forward<Args>(args)...);
      }

      BOOST_FORCEINLINE size_type erase(key_type const& k)
      {
        return table_.erase(k);
      }

      template <class K>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value, size_type>::type
      erase(K&& k)
      {
        return table_.erase(std::forward<K>(k));
      }

      template <class F>
      BOOST_FORCEINLINE size_type erase_if(key_type const& k, F f)
      {
        return table_.erase_if(k, f);
      }

      template <class K, class F>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value &&
          !detail::is_execution_policy<K>::value,
        size_type>::type
      erase_if(K&& k, F f)
      {
        return table_.erase_if(std::forward<K>(k), f);
      }

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)
      template <class ExecPolicy, class F>
      typename std::enable_if<detail::is_execution_policy<ExecPolicy>::value,
        void>::type
      erase_if(ExecPolicy&& p, F f)
      {
        BOOST_UNORDERED_STATIC_ASSERT_EXEC_POLICY(ExecPolicy)
        table_.erase_if(p, f);
      }
#endif

      template <class F> size_type erase_if(F f) { return table_.erase_if(f); }

      void swap(concurrent_flat_map& other) noexcept(
        boost::allocator_is_always_equal<Allocator>::type::value ||
        boost::allocator_propagate_on_container_swap<Allocator>::type::value)
      {
        return table_.swap(other.table_);
      }

      void clear() noexcept { table_.clear(); }

      template <typename H2, typename P2>
      size_type merge(concurrent_flat_map<Key, T, H2, P2, Allocator>& x)
      {
        BOOST_ASSERT(get_allocator() == x.get_allocator());
        return table_.merge(x.table_);
      }

      template <typename H2, typename P2>
      size_type merge(concurrent_flat_map<Key, T, H2, P2, Allocator>&& x)
      {
        return merge(x);
      }

      BOOST_FORCEINLINE size_type count(key_type const& k) const
      {
        return table_.count(k);
      }

      template <class K>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value, size_type>::type
      count(K const& k)
      {
        return table_.count(k);
      }

      BOOST_FORCEINLINE bool contains(key_type const& k) const
      {
        return table_.contains(k);
      }

      template <class K>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value, bool>::type
      contains(K const& k) const
      {
        return table_.contains(k);
      }

      /// Hash Policy
      ///
      size_type bucket_count() const noexcept { return table_.capacity(); }

      float load_factor() const noexcept { return table_.load_factor(); }
      float max_load_factor() const noexcept
      {
        return table_.max_load_factor();
      }
      void max_load_factor(float) {}
      size_type max_load() const noexcept { return table_.max_load(); }

      void rehash(size_type n) { table_.rehash(n); }
      void reserve(size_type n) { table_.reserve(n); }

#if defined(BOOST_UNORDERED_ENABLE_STATS)
      /// Stats
      ///
      stats get_stats() const { return table_.get_stats(); }

      void reset_stats() noexcept { table_.reset_stats(); }
#endif

      /// Observers
      ///
      allocator_type get_allocator() const noexcept
      {
        return table_.get_allocator();
      }

      hasher hash_function() const { return table_.hash_function(); }
      key_equal key_eq() const { return table_.key_eq(); }
    };

    template <class Key, class T, class Hash, class KeyEqual, class Allocator>
    bool operator==(
      concurrent_flat_map<Key, T, Hash, KeyEqual, Allocator> const& lhs,
      concurrent_flat_map<Key, T, Hash, KeyEqual, Allocator> const& rhs)
    {
      return lhs.table_ == rhs.table_;
    }

    template <class Key, class T, class Hash, class KeyEqual, class Allocator>
    bool operator!=(
      concurrent_flat_map<Key, T, Hash, KeyEqual, Allocator> const& lhs,
      concurrent_flat_map<Key, T, Hash, KeyEqual, Allocator> const& rhs)
    {
      return !(lhs == rhs);
    }

    template <class Key, class T, class Hash, class Pred, class Alloc>
    void swap(concurrent_flat_map<Key, T, Hash, Pred, Alloc>& x,
      concurrent_flat_map<Key, T, Hash, Pred, Alloc>& y)
      noexcept(noexcept(x.swap(y)))
    {
      x.swap(y);
    }

    template <class K, class T, class H, class P, class A, class Predicate>
    typename concurrent_flat_map<K, T, H, P, A>::size_type erase_if(
      concurrent_flat_map<K, T, H, P, A>& c, Predicate pred)
    {
      return c.table_.erase_if(pred);
    }

    template<class Archive, class K, class V, class H, class KE, class A>
    void serialize(
      Archive& ar, concurrent_flat_map<K, V, H, KE, A>& c, unsigned int)
    {
      ar & core::make_nvp("table",c.table_);
    }

#if BOOST_UNORDERED_TEMPLATE_DEDUCTION_GUIDES

    template <class InputIterator,
      class Hash =
        boost::hash<boost::unordered::detail::iter_key_t<InputIterator> >,
      class Pred =
        std::equal_to<boost::unordered::detail::iter_key_t<InputIterator> >,
      class Allocator = std::allocator<
        boost::unordered::detail::iter_to_alloc_t<InputIterator> >,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_pred_v<Pred> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    concurrent_flat_map(InputIterator, InputIterator,
      std::size_t = boost::unordered::detail::foa::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> concurrent_flat_map<
        boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>, Hash, Pred,
        Allocator>;

    template <class Key, class T,
      class Hash = boost::hash<std::remove_const_t<Key> >,
      class Pred = std::equal_to<std::remove_const_t<Key> >,
      class Allocator = std::allocator<std::pair<const Key, T> >,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_pred_v<Pred> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    concurrent_flat_map(std::initializer_list<std::pair<Key, T> >,
      std::size_t = boost::unordered::detail::foa::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> concurrent_flat_map<std::remove_const_t<Key>, T, Hash, Pred,
        Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    concurrent_flat_map(InputIterator, InputIterator, std::size_t, Allocator)
      -> concurrent_flat_map<
        boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>,
        boost::hash<boost::unordered::detail::iter_key_t<InputIterator> >,
        std::equal_to<boost::unordered::detail::iter_key_t<InputIterator> >,
        Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    concurrent_flat_map(InputIterator, InputIterator, Allocator)
      -> concurrent_flat_map<
        boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>,
        boost::hash<boost::unordered::detail::iter_key_t<InputIterator> >,
        std::equal_to<boost::unordered::detail::iter_key_t<InputIterator> >,
        Allocator>;

    template <class InputIterator, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    concurrent_flat_map(
      InputIterator, InputIterator, std::size_t, Hash, Allocator)
      -> concurrent_flat_map<
        boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>, Hash,
        std::equal_to<boost::unordered::detail::iter_key_t<InputIterator> >,
        Allocator>;

    template <class Key, class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    concurrent_flat_map(std::initializer_list<std::pair<Key, T> >, std::size_t,
      Allocator) -> concurrent_flat_map<std::remove_const_t<Key>, T,
      boost::hash<std::remove_const_t<Key> >,
      std::equal_to<std::remove_const_t<Key> >, Allocator>;

    template <class Key, class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    concurrent_flat_map(std::initializer_list<std::pair<Key, T> >, Allocator)
      -> concurrent_flat_map<std::remove_const_t<Key>, T,
        boost::hash<std::remove_const_t<Key> >,
        std::equal_to<std::remove_const_t<Key> >, Allocator>;

    template <class Key, class T, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    concurrent_flat_map(std::initializer_list<std::pair<Key, T> >, std::size_t,
      Hash, Allocator) -> concurrent_flat_map<std::remove_const_t<Key>, T,
      Hash, std::equal_to<std::remove_const_t<Key> >, Allocator>;

#endif

  } // namespace unordered
} // namespace boost

#endif // BOOST_UNORDERED_CONCURRENT_FLAT_MAP_HPP
