// Copyright (C) 2022-2023 Christian Mazakas
// Copyright (C) 2024 Joaquin M Lopez Munoz
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_UNORDERED_FLAT_SET_HPP_INCLUDED
#define BOOST_UNORDERED_UNORDERED_FLAT_SET_HPP_INCLUDED

#include <boost/config.hpp>
#if defined(BOOST_HAS_PRAGMA_ONCE)
#pragma once
#endif

#include <boost/unordered/concurrent_flat_set_fwd.hpp>
#include <boost/unordered/detail/foa/flat_set_types.hpp>
#include <boost/unordered/detail/foa/table.hpp>
#include <boost/unordered/detail/serialize_container.hpp>
#include <boost/unordered/detail/type_traits.hpp>
#include <boost/unordered/unordered_flat_set_fwd.hpp>

#include <boost/core/allocator_access.hpp>
#include <boost/container_hash/hash.hpp>

#include <initializer_list>
#include <iterator>
#include <type_traits>
#include <utility>

namespace boost {
  namespace unordered {

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable : 4714) /* marked as __forceinline not inlined */
#endif

    template <class Key, class Hash, class KeyEqual, class Allocator>
    class unordered_flat_set
    {
      template <class Key2, class Hash2, class KeyEqual2, class Allocator2>
      friend class concurrent_flat_set;

      using set_types = detail::foa::flat_set_types<Key>;

      using table_type = detail::foa::table<set_types, Hash, KeyEqual,
        typename boost::allocator_rebind<Allocator,
          typename set_types::value_type>::type>;

      table_type table_;

      template <class K, class H, class KE, class A>
      bool friend operator==(unordered_flat_set<K, H, KE, A> const& lhs,
        unordered_flat_set<K, H, KE, A> const& rhs);

      template <class K, class H, class KE, class A, class Pred>
      typename unordered_flat_set<K, H, KE, A>::size_type friend erase_if(
        unordered_flat_set<K, H, KE, A>& set, Pred pred);

    public:
      using key_type = Key;
      using value_type = typename set_types::value_type;
      using init_type = typename set_types::init_type;
      using size_type = std::size_t;
      using difference_type = std::ptrdiff_t;
      using hasher = Hash;
      using key_equal = KeyEqual;
      using allocator_type = Allocator;
      using reference = value_type&;
      using const_reference = value_type const&;
      using pointer = typename boost::allocator_pointer<allocator_type>::type;
      using const_pointer =
        typename boost::allocator_const_pointer<allocator_type>::type;
      using iterator = typename table_type::iterator;
      using const_iterator = typename table_type::const_iterator;

#if defined(BOOST_UNORDERED_ENABLE_STATS)
      using stats = typename table_type::stats;
#endif

      unordered_flat_set() : unordered_flat_set(0) {}

      explicit unordered_flat_set(size_type n, hasher const& h = hasher(),
        key_equal const& pred = key_equal(),
        allocator_type const& a = allocator_type())
          : table_(n, h, pred, a)
      {
      }

      unordered_flat_set(size_type n, allocator_type const& a)
          : unordered_flat_set(n, hasher(), key_equal(), a)
      {
      }

      unordered_flat_set(size_type n, hasher const& h, allocator_type const& a)
          : unordered_flat_set(n, h, key_equal(), a)
      {
      }

      template <class InputIterator>
      unordered_flat_set(
        InputIterator f, InputIterator l, allocator_type const& a)
          : unordered_flat_set(f, l, size_type(0), hasher(), key_equal(), a)
      {
      }

      explicit unordered_flat_set(allocator_type const& a)
          : unordered_flat_set(0, a)
      {
      }

      template <class Iterator>
      unordered_flat_set(Iterator first, Iterator last, size_type n = 0,
        hasher const& h = hasher(), key_equal const& pred = key_equal(),
        allocator_type const& a = allocator_type())
          : unordered_flat_set(n, h, pred, a)
      {
        this->insert(first, last);
      }

      template <class InputIt>
      unordered_flat_set(
        InputIt first, InputIt last, size_type n, allocator_type const& a)
          : unordered_flat_set(first, last, n, hasher(), key_equal(), a)
      {
      }

      template <class Iterator>
      unordered_flat_set(Iterator first, Iterator last, size_type n,
        hasher const& h, allocator_type const& a)
          : unordered_flat_set(first, last, n, h, key_equal(), a)
      {
      }

      unordered_flat_set(unordered_flat_set const& other) : table_(other.table_)
      {
      }

      unordered_flat_set(
        unordered_flat_set const& other, allocator_type const& a)
          : table_(other.table_, a)
      {
      }

      unordered_flat_set(unordered_flat_set&& other)
        noexcept(std::is_nothrow_move_constructible<table_type>::value)
          : table_(std::move(other.table_))
      {
      }

      unordered_flat_set(unordered_flat_set&& other, allocator_type const& al)
          : table_(std::move(other.table_), al)
      {
      }

      unordered_flat_set(std::initializer_list<value_type> ilist,
        size_type n = 0, hasher const& h = hasher(),
        key_equal const& pred = key_equal(),
        allocator_type const& a = allocator_type())
          : unordered_flat_set(ilist.begin(), ilist.end(), n, h, pred, a)
      {
      }

      unordered_flat_set(
        std::initializer_list<value_type> il, allocator_type const& a)
          : unordered_flat_set(il, size_type(0), hasher(), key_equal(), a)
      {
      }

      unordered_flat_set(std::initializer_list<value_type> init, size_type n,
        allocator_type const& a)
          : unordered_flat_set(init, n, hasher(), key_equal(), a)
      {
      }

      unordered_flat_set(std::initializer_list<value_type> init, size_type n,
        hasher const& h, allocator_type const& a)
          : unordered_flat_set(init, n, h, key_equal(), a)
      {
      }

      template <bool avoid_explicit_instantiation = true>
      unordered_flat_set(
        concurrent_flat_set<Key, Hash, KeyEqual, Allocator>&& other)
          : table_(std::move(other.table_))
      {
      }

      ~unordered_flat_set() = default;

      unordered_flat_set& operator=(unordered_flat_set const& other)
      {
        table_ = other.table_;
        return *this;
      }

      unordered_flat_set& operator=(unordered_flat_set&& other) noexcept(
        noexcept(std::declval<table_type&>() = std::declval<table_type&&>()))
      {
        table_ = std::move(other.table_);
        return *this;
      }

      unordered_flat_set& operator=(std::initializer_list<value_type> il)
      {
        this->clear();
        this->insert(il.begin(), il.end());
        return *this;
      }

      allocator_type get_allocator() const noexcept
      {
        return table_.get_allocator();
      }

      /// Iterators
      ///

      iterator begin() noexcept { return table_.begin(); }
      const_iterator begin() const noexcept { return table_.begin(); }
      const_iterator cbegin() const noexcept { return table_.cbegin(); }

      iterator end() noexcept { return table_.end(); }
      const_iterator end() const noexcept { return table_.end(); }
      const_iterator cend() const noexcept { return table_.cend(); }

      /// Capacity
      ///

      BOOST_ATTRIBUTE_NODISCARD bool empty() const noexcept
      {
        return table_.empty();
      }

      size_type size() const noexcept { return table_.size(); }

      size_type max_size() const noexcept { return table_.max_size(); }

      /// Modifiers
      ///

      void clear() noexcept { table_.clear(); }

      BOOST_FORCEINLINE std::pair<iterator, bool> insert(
        value_type const& value)
      {
        return table_.insert(value);
      }

      BOOST_FORCEINLINE std::pair<iterator, bool> insert(value_type&& value)
      {
        return table_.insert(std::move(value));
      }

      template <class K>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::transparent_non_iterable<K, unordered_flat_set>::value,
        std::pair<iterator, bool> >::type
      insert(K&& k)
      {
        return table_.try_emplace(std::forward<K>(k));
      }

      BOOST_FORCEINLINE iterator insert(const_iterator, value_type const& value)
      {
        return table_.insert(value).first;
      }

      BOOST_FORCEINLINE iterator insert(const_iterator, value_type&& value)
      {
        return table_.insert(std::move(value)).first;
      }

      template <class K>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::transparent_non_iterable<K, unordered_flat_set>::value,
        iterator>::type
      insert(const_iterator, K&& k)
      {
        return table_.try_emplace(std::forward<K>(k)).first;
      }

      template <class InputIterator>
      void insert(InputIterator first, InputIterator last)
      {
        for (auto pos = first; pos != last; ++pos) {
          table_.emplace(*pos);
        }
      }

      void insert(std::initializer_list<value_type> ilist)
      {
        this->insert(ilist.begin(), ilist.end());
      }

      template <class... Args>
      BOOST_FORCEINLINE std::pair<iterator, bool> emplace(Args&&... args)
      {
        return table_.emplace(std::forward<Args>(args)...);
      }

      template <class... Args>
      BOOST_FORCEINLINE iterator emplace_hint(const_iterator, Args&&... args)
      {
        return table_.emplace(std::forward<Args>(args)...).first;
      }

      BOOST_FORCEINLINE typename table_type::erase_return_type erase(
        const_iterator pos)
      {
        return table_.erase(pos);
      }

      iterator erase(const_iterator first, const_iterator last)
      {
        while (first != last) {
          this->erase(first++);
        }
        return iterator{detail::foa::const_iterator_cast_tag{}, last};
      }

      BOOST_FORCEINLINE size_type erase(key_type const& key)
      {
        return table_.erase(key);
      }

      template <class K>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::transparent_non_iterable<K, unordered_flat_set>::value,
        size_type>::type
      erase(K const& key)
      {
        return table_.erase(key);
      }

      void swap(unordered_flat_set& rhs) noexcept(
        noexcept(std::declval<table_type&>().swap(std::declval<table_type&>())))
      {
        table_.swap(rhs.table_);
      }

      template <class H2, class P2>
      void merge(unordered_flat_set<key_type, H2, P2, allocator_type>& source)
      {
        table_.merge(source.table_);
      }

      template <class H2, class P2>
      void merge(unordered_flat_set<key_type, H2, P2, allocator_type>&& source)
      {
        table_.merge(std::move(source.table_));
      }

      /// Lookup
      ///

      BOOST_FORCEINLINE size_type count(key_type const& key) const
      {
        auto pos = table_.find(key);
        return pos != table_.end() ? 1 : 0;
      }

      template <class K>
      BOOST_FORCEINLINE typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value, size_type>::type
      count(K const& key) const
      {
        auto pos = table_.find(key);
        return pos != table_.end() ? 1 : 0;
      }

      BOOST_FORCEINLINE iterator find(key_type const& key)
      {
        return table_.find(key);
      }

      BOOST_FORCEINLINE const_iterator find(key_type const& key) const
      {
        return table_.find(key);
      }

      template <class K>
      BOOST_FORCEINLINE typename std::enable_if<
        boost::unordered::detail::are_transparent<K, hasher, key_equal>::value,
        iterator>::type
      find(K const& key)
      {
        return table_.find(key);
      }

      template <class K>
      BOOST_FORCEINLINE typename std::enable_if<
        boost::unordered::detail::are_transparent<K, hasher, key_equal>::value,
        const_iterator>::type
      find(K const& key) const
      {
        return table_.find(key);
      }

      BOOST_FORCEINLINE bool contains(key_type const& key) const
      {
        return this->find(key) != this->end();
      }

      template <class K>
      BOOST_FORCEINLINE typename std::enable_if<
        boost::unordered::detail::are_transparent<K, hasher, key_equal>::value,
        bool>::type
      contains(K const& key) const
      {
        return this->find(key) != this->end();
      }

      std::pair<iterator, iterator> equal_range(key_type const& key)
      {
        auto pos = table_.find(key);
        if (pos == table_.end()) {
          return {pos, pos};
        }

        auto next = pos;
        ++next;
        return {pos, next};
      }

      std::pair<const_iterator, const_iterator> equal_range(
        key_type const& key) const
      {
        auto pos = table_.find(key);
        if (pos == table_.end()) {
          return {pos, pos};
        }

        auto next = pos;
        ++next;
        return {pos, next};
      }

      template <class K>
      typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value,
        std::pair<iterator, iterator> >::type
      equal_range(K const& key)
      {
        auto pos = table_.find(key);
        if (pos == table_.end()) {
          return {pos, pos};
        }

        auto next = pos;
        ++next;
        return {pos, next};
      }

      template <class K>
      typename std::enable_if<
        detail::are_transparent<K, hasher, key_equal>::value,
        std::pair<const_iterator, const_iterator> >::type
      equal_range(K const& key) const
      {
        auto pos = table_.find(key);
        if (pos == table_.end()) {
          return {pos, pos};
        }

        auto next = pos;
        ++next;
        return {pos, next};
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

      hasher hash_function() const { return table_.hash_function(); }

      key_equal key_eq() const { return table_.key_eq(); }
    };

    template <class Key, class Hash, class KeyEqual, class Allocator>
    bool operator==(
      unordered_flat_set<Key, Hash, KeyEqual, Allocator> const& lhs,
      unordered_flat_set<Key, Hash, KeyEqual, Allocator> const& rhs)
    {
      return lhs.table_ == rhs.table_;
    }

    template <class Key, class Hash, class KeyEqual, class Allocator>
    bool operator!=(
      unordered_flat_set<Key, Hash, KeyEqual, Allocator> const& lhs,
      unordered_flat_set<Key, Hash, KeyEqual, Allocator> const& rhs)
    {
      return !(lhs == rhs);
    }

    template <class Key, class Hash, class KeyEqual, class Allocator>
    void swap(unordered_flat_set<Key, Hash, KeyEqual, Allocator>& lhs,
      unordered_flat_set<Key, Hash, KeyEqual, Allocator>& rhs)
      noexcept(noexcept(lhs.swap(rhs)))
    {
      lhs.swap(rhs);
    }

    template <class Key, class Hash, class KeyEqual, class Allocator,
      class Pred>
    typename unordered_flat_set<Key, Hash, KeyEqual, Allocator>::size_type
    erase_if(unordered_flat_set<Key, Hash, KeyEqual, Allocator>& set, Pred pred)
    {
      return erase_if(set.table_, pred);
    }

    template <class Archive, class Key, class Hash, class KeyEqual,
      class Allocator>
    void serialize(Archive& ar,
      unordered_flat_set<Key, Hash, KeyEqual, Allocator>& set,
      unsigned int version)
    {
      detail::serialize_container(ar, set, version);
    }

#if defined(BOOST_MSVC)
#pragma warning(pop) /* C4714 */
#endif

#if BOOST_UNORDERED_TEMPLATE_DEDUCTION_GUIDES
    template <class InputIterator,
      class Hash =
        boost::hash<typename std::iterator_traits<InputIterator>::value_type>,
      class Pred =
        std::equal_to<typename std::iterator_traits<InputIterator>::value_type>,
      class Allocator = std::allocator<
        typename std::iterator_traits<InputIterator>::value_type>,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_pred_v<Pred> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_flat_set(InputIterator, InputIterator,
      std::size_t = boost::unordered::detail::foa::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> unordered_flat_set<
        typename std::iterator_traits<InputIterator>::value_type, Hash, Pred,
        Allocator>;

    template <class T, class Hash = boost::hash<T>,
      class Pred = std::equal_to<T>, class Allocator = std::allocator<T>,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_pred_v<Pred> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_flat_set(std::initializer_list<T>,
      std::size_t = boost::unordered::detail::foa::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> unordered_flat_set<T, Hash, Pred, Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_flat_set(InputIterator, InputIterator, std::size_t, Allocator)
      -> unordered_flat_set<
        typename std::iterator_traits<InputIterator>::value_type,
        boost::hash<typename std::iterator_traits<InputIterator>::value_type>,
        std::equal_to<typename std::iterator_traits<InputIterator>::value_type>,
        Allocator>;

    template <class InputIterator, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_flat_set(
      InputIterator, InputIterator, std::size_t, Hash, Allocator)
      -> unordered_flat_set<
        typename std::iterator_traits<InputIterator>::value_type, Hash,
        std::equal_to<typename std::iterator_traits<InputIterator>::value_type>,
        Allocator>;

    template <class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_flat_set(std::initializer_list<T>, std::size_t, Allocator)
      -> unordered_flat_set<T, boost::hash<T>, std::equal_to<T>, Allocator>;

    template <class T, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_flat_set(std::initializer_list<T>, std::size_t, Hash, Allocator)
      -> unordered_flat_set<T, Hash, std::equal_to<T>, Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_flat_set(InputIterator, InputIterator, Allocator)
      -> unordered_flat_set<
        typename std::iterator_traits<InputIterator>::value_type,
        boost::hash<typename std::iterator_traits<InputIterator>::value_type>,
        std::equal_to<typename std::iterator_traits<InputIterator>::value_type>,
        Allocator>;

    template <class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_flat_set(std::initializer_list<T>, Allocator)
      -> unordered_flat_set<T, boost::hash<T>, std::equal_to<T>, Allocator>;
#endif

  } // namespace unordered
} // namespace boost

#endif
