// Copyright (C) 2003-2004 Jeremy B. Maitin-Shepard.
// Copyright (C) 2005-2011 Daniel James.
// Copyright (C) 2022-2023 Christian Mazakas
// Copyright (C) 2024 Joaquin M Lopez Munoz.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/unordered for documentation

#ifndef BOOST_UNORDERED_UNORDERED_MAP_HPP_INCLUDED
#define BOOST_UNORDERED_UNORDERED_MAP_HPP_INCLUDED

#include <boost/config.hpp>
#if defined(BOOST_HAS_PRAGMA_ONCE)
#pragma once
#endif

#include <boost/unordered/detail/map.hpp>
#include <boost/unordered/detail/serialize_fca_container.hpp>
#include <boost/unordered/detail/throw_exception.hpp>
#include <boost/unordered/detail/type_traits.hpp>

#include <boost/container_hash/hash.hpp>

#include <initializer_list>

#if defined(BOOST_MSVC)
#pragma warning(push)
// conditional expression is constant
#pragma warning(disable : 4127)
#if BOOST_MSVC >= 1400
// the inline specifier cannot be used when a friend declaration refers to a
// specialization of a function template
#pragma warning(disable : 4396)
#endif
#endif

namespace boost {
  namespace unordered {
    template <class K, class T, class H, class P, class A> class unordered_map
    {
      template <typename, typename, typename, typename, typename>
      friend class unordered_multimap;

    public:
      typedef K key_type;
      typedef T mapped_type;
      typedef std::pair<const K, T> value_type;
      typedef typename boost::unordered::detail::type_identity<H>::type hasher;
      typedef
        typename boost::unordered::detail::type_identity<P>::type key_equal;
      typedef typename boost::unordered::detail::type_identity<A>::type
        allocator_type;

    private:
      typedef boost::unordered::detail::map<A, K, T, H, P> types;
      typedef typename types::value_allocator_traits value_allocator_traits;
      typedef typename types::table table;

    public:
      typedef typename value_allocator_traits::pointer pointer;
      typedef typename value_allocator_traits::const_pointer const_pointer;

      typedef value_type& reference;
      typedef value_type const& const_reference;

      typedef std::size_t size_type;
      typedef std::ptrdiff_t difference_type;

      typedef typename table::iterator iterator;
      typedef typename table::c_iterator const_iterator;
      typedef typename table::l_iterator local_iterator;
      typedef typename table::cl_iterator const_local_iterator;
      typedef typename types::node_type node_type;
      typedef typename types::insert_return_type insert_return_type;

    private:
      table table_;

    public:
      // constructors

      unordered_map();

      explicit unordered_map(size_type, const hasher& = hasher(),
        const key_equal& = key_equal(),
        const allocator_type& = allocator_type());

      template <class InputIt>
      unordered_map(InputIt, InputIt,
        size_type = boost::unordered::detail::default_bucket_count,
        const hasher& = hasher(), const key_equal& = key_equal(),
        const allocator_type& = allocator_type());

      unordered_map(unordered_map const&);

      unordered_map(unordered_map&& other)
        noexcept(table::nothrow_move_constructible)
          : table_(other.table_, boost::unordered::detail::move_tag())
      {
        // The move is done in table_
      }

      explicit unordered_map(allocator_type const&);

      unordered_map(unordered_map const&, allocator_type const&);

      unordered_map(unordered_map&&, allocator_type const&);

      unordered_map(std::initializer_list<value_type>,
        size_type = boost::unordered::detail::default_bucket_count,
        const hasher& = hasher(), const key_equal& l = key_equal(),
        const allocator_type& = allocator_type());

      explicit unordered_map(size_type, const allocator_type&);

      explicit unordered_map(size_type, const hasher&, const allocator_type&);

      template <class InputIterator>
      unordered_map(InputIterator, InputIterator, const allocator_type&);

      template <class InputIt>
      unordered_map(InputIt, InputIt, size_type, const allocator_type&);

      template <class InputIt>
      unordered_map(
        InputIt, InputIt, size_type, const hasher&, const allocator_type&);

      unordered_map(std::initializer_list<value_type>, const allocator_type&);

      unordered_map(
        std::initializer_list<value_type>, size_type, const allocator_type&);

      unordered_map(std::initializer_list<value_type>, size_type, const hasher&,
        const allocator_type&);

      // Destructor

      ~unordered_map() noexcept;

      // Assign

      unordered_map& operator=(unordered_map const& x)
      {
        table_.assign(x.table_, std::true_type());
        return *this;
      }

      unordered_map& operator=(unordered_map&& x)
        noexcept(value_allocator_traits::is_always_equal::value&&
            std::is_nothrow_move_assignable<H>::value&&
              std::is_nothrow_move_assignable<P>::value)
      {
        table_.move_assign(x.table_, std::true_type());
        return *this;
      }

      unordered_map& operator=(std::initializer_list<value_type>);

      allocator_type get_allocator() const noexcept
      {
        return allocator_type(table_.node_alloc());
      }

      //       // iterators

      iterator begin() noexcept { return table_.begin(); }

      const_iterator begin() const noexcept
      {
        return const_iterator(table_.begin());
      }

      iterator end() noexcept { return iterator(); }

      const_iterator end() const noexcept { return const_iterator(); }

      const_iterator cbegin() const noexcept
      {
        return const_iterator(table_.begin());
      }

      const_iterator cend() const noexcept { return const_iterator(); }

      // size and capacity

      BOOST_ATTRIBUTE_NODISCARD bool empty() const noexcept
      {
        return table_.size_ == 0;
      }

      size_type size() const noexcept { return table_.size_; }

      size_type max_size() const noexcept;

      // emplace

      template <class... Args> std::pair<iterator, bool> emplace(Args&&... args)
      {
        return table_.emplace_unique(
          table::extractor::extract(detail::as_const(args)...),
          std::forward<Args>(args)...);
      }

      template <class... Args>
      iterator emplace_hint(const_iterator hint, Args&&... args)
      {
        return table_.emplace_hint_unique(hint,
          table::extractor::extract(detail::as_const(args)...),
          std::forward<Args>(args)...);
      }

      std::pair<iterator, bool> insert(value_type const& x)
      {
        return this->emplace(x);
      }

      std::pair<iterator, bool> insert(value_type&& x)
      {
        return this->emplace(std::move(x));
      }

      template <class P2>
      typename boost::enable_if<std::is_constructible<value_type, P2&&>,
        std::pair<iterator, bool> >::type
      insert(P2&& obj)
      {
        return this->emplace(std::forward<P2>(obj));
      }

      iterator insert(const_iterator hint, value_type const& x)
      {
        return this->emplace_hint(hint, x);
      }

      iterator insert(const_iterator hint, value_type&& x)
      {
        return this->emplace_hint(hint, std::move(x));
      }

      template <class P2>
      typename boost::enable_if<std::is_constructible<value_type, P2&&>,
        iterator>::type
      insert(const_iterator hint, P2&& obj)
      {
        return this->emplace_hint(hint, std::forward<P2>(obj));
      }

      template <class InputIt> void insert(InputIt, InputIt);

      void insert(std::initializer_list<value_type>);

      // extract

      node_type extract(const_iterator position)
      {
        return node_type(
          table_.extract_by_iterator_unique(position),
          allocator_type(table_.node_alloc()));
      }

      node_type extract(const key_type& k)
      {
        return node_type(
          table_.extract_by_key_impl(k),
          allocator_type(table_.node_alloc()));
      }

      template <class Key>
      typename boost::enable_if_c<
        detail::transparent_non_iterable<Key, unordered_map>::value,
        node_type>::type
      extract(Key&& k)
      {
        return node_type(
          table_.extract_by_key_impl(std::forward<Key>(k)),
          allocator_type(table_.node_alloc()));
      }

      insert_return_type insert(node_type&& np)
      {
        insert_return_type result;
        table_.move_insert_node_type_unique((node_type&)np, result);
        return result;
      }

      iterator insert(const_iterator hint, node_type&& np)
      {
        return table_.move_insert_node_type_with_hint_unique(hint, np);
      }

      template <class... Args>
      std::pair<iterator, bool> try_emplace(key_type const& k, Args&&... args)
      {
        return table_.try_emplace_unique(k, std::forward<Args>(args)...);
      }

      template <class... Args>
      std::pair<iterator, bool> try_emplace(key_type&& k, Args&&... args)
      {
        return table_.try_emplace_unique(
          std::move(k), std::forward<Args>(args)...);
      }

      template <class Key, class... Args>
      typename boost::enable_if_c<
        detail::transparent_non_iterable<Key, unordered_map>::value,
        std::pair<iterator, bool> >::type
      try_emplace(Key&& k, Args&&... args)
      {
        return table_.try_emplace_unique(
          std::forward<Key>(k), std::forward<Args>(args)...);
      }

      template <class... Args>
      iterator try_emplace(
        const_iterator hint, key_type const& k, Args&&... args)
      {
        return table_.try_emplace_hint_unique(
          hint, k, std::forward<Args>(args)...);
      }

      template <class... Args>
      iterator try_emplace(const_iterator hint, key_type&& k, Args&&... args)
      {
        return table_.try_emplace_hint_unique(
          hint, std::move(k), std::forward<Args>(args)...);
      }

      template <class Key, class... Args>
      typename boost::enable_if_c<
        detail::transparent_non_iterable<Key, unordered_map>::value,
        iterator>::type
      try_emplace(const_iterator hint, Key&& k, Args&&... args)
      {
        return table_.try_emplace_hint_unique(
          hint, std::forward<Key>(k), std::forward<Args>(args)...);
      }

      template <class M>
      std::pair<iterator, bool> insert_or_assign(key_type const& k, M&& obj)
      {
        return table_.insert_or_assign_unique(k, std::forward<M>(obj));
      }

      template <class M>
      std::pair<iterator, bool> insert_or_assign(key_type&& k, M&& obj)
      {
        return table_.insert_or_assign_unique(
          std::move(k), std::forward<M>(obj));
      }

      template <class Key, class M>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        std::pair<iterator, bool> >::type
      insert_or_assign(Key&& k, M&& obj)
      {
        return table_.insert_or_assign_unique(
          std::forward<Key>(k), std::forward<M>(obj));
      }

      template <class M>
      iterator insert_or_assign(const_iterator, key_type const& k, M&& obj)
      {
        return table_.insert_or_assign_unique(k, std::forward<M>(obj)).first;
      }

      template <class M>
      iterator insert_or_assign(const_iterator, key_type&& k, M&& obj)
      {
        return table_
          .insert_or_assign_unique(std::move(k), std::forward<M>(obj))
          .first;
      }

      template <class Key, class M>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        iterator>::type
      insert_or_assign(const_iterator, Key&& k, M&& obj)
      {
        return table_
          .insert_or_assign_unique(std::forward<Key>(k), std::forward<M>(obj))
          .first;
      }

      iterator erase(iterator);
      iterator erase(const_iterator);
      size_type erase(const key_type&);
      iterator erase(const_iterator, const_iterator);

      template <class Key>
      typename boost::enable_if_c<
        detail::transparent_non_iterable<Key, unordered_map>::value,
        size_type>::type
      erase(Key&& k)
      {
        return table_.erase_key_unique_impl(std::forward<Key>(k));
      }

      BOOST_UNORDERED_DEPRECATED("Use erase instead")
      void quick_erase(const_iterator it) { erase(it); }
      BOOST_UNORDERED_DEPRECATED("Use erase instead")
      void erase_return_void(const_iterator it) { erase(it); }

      void swap(unordered_map&)
        noexcept(value_allocator_traits::is_always_equal::value&&
            boost::unordered::detail::is_nothrow_swappable<H>::value&&
              boost::unordered::detail::is_nothrow_swappable<P>::value);
      void clear() noexcept { table_.clear_impl(); }

      template <typename H2, typename P2>
      void merge(boost::unordered_map<K, T, H2, P2, A>& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_map<K, T, H2, P2, A>&& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_multimap<K, T, H2, P2, A>& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_multimap<K, T, H2, P2, A>&& source);

      // observers

      hasher hash_function() const;
      key_equal key_eq() const;

      // lookup

      iterator find(const key_type&);
      const_iterator find(const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        iterator>::type
      find(const Key& key)
      {
        return table_.find(key);
      }

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        const_iterator>::type
      find(const Key& key) const
      {
        return const_iterator(table_.find(key));
      }

      template <class CompatibleKey, class CompatibleHash,
        class CompatiblePredicate>
      iterator find(CompatibleKey const&, CompatibleHash const&,
        CompatiblePredicate const&);

      template <class CompatibleKey, class CompatibleHash,
        class CompatiblePredicate>
      const_iterator find(CompatibleKey const&, CompatibleHash const&,
        CompatiblePredicate const&) const;

      bool contains(const key_type& k) const
      {
        return table_.find(k) != this->end();
      }

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        bool>::type
      contains(const Key& k) const
      {
        return table_.find(k) != this->end();
      }

      size_type count(const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        size_type>::type
      count(const Key& k) const
      {
        return (table_.find(k) != this->end() ? 1 : 0);
      }

      std::pair<iterator, iterator> equal_range(const key_type&);
      std::pair<const_iterator, const_iterator> equal_range(
        const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        std::pair<iterator, iterator> >::type
      equal_range(const Key& key)
      {
        iterator first = table_.find(key);
        iterator last = first;
        if (last != this->end()) {
          ++last;
        }

        return std::make_pair(first, last);
      }

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        std::pair<const_iterator, const_iterator> >::type
      equal_range(const Key& key) const
      {
        iterator first = table_.find(key);
        iterator last = first;
        if (last != this->end()) {
          ++last;
        }

        return std::make_pair(first, last);
      }

      mapped_type& operator[](const key_type&);
      mapped_type& operator[](key_type&&);

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        mapped_type&>::type
      operator[](Key&& k);

      mapped_type& at(const key_type&);
      mapped_type const& at(const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        mapped_type&>::type
      at(Key&& k);

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        mapped_type const&>::type
      at(Key&& k) const;

      // bucket interface

      size_type bucket_count() const noexcept { return table_.bucket_count(); }

      size_type max_bucket_count() const noexcept
      {
        return table_.max_bucket_count();
      }

      size_type bucket_size(size_type) const;

      size_type bucket(const key_type& k) const
      {
        return table_.hash_to_bucket(table_.hash(k));
      }

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        size_type>::type
      bucket(Key&& k) const
      {
        return table_.hash_to_bucket(table_.hash(std::forward<Key>(k)));
      }

      local_iterator begin(size_type n) { return table_.begin(n); }

      const_local_iterator begin(size_type n) const
      {
        return const_local_iterator(table_.begin(n));
      }

      local_iterator end(size_type) { return local_iterator(); }
      const_local_iterator end(size_type) const
      {
        return const_local_iterator();
      }

      const_local_iterator cbegin(size_type n) const
      {
        return const_local_iterator(table_.begin(n));
      }

      const_local_iterator cend(size_type) const
      {
        return const_local_iterator();
      }

      // hash policy

      float load_factor() const noexcept;
      float max_load_factor() const noexcept { return table_.mlf_; }
      void max_load_factor(float) noexcept;
      void rehash(size_type);
      void reserve(size_type);

#if !BOOST_WORKAROUND(BOOST_BORLANDC, < 0x0582)
      friend bool operator==
        <K, T, H, P, A>(unordered_map const&, unordered_map const&);
      friend bool operator!=
        <K, T, H, P, A>(unordered_map const&, unordered_map const&);
#endif
    }; // class template unordered_map

    template <class Archive, class K, class T, class H, class P, class A>
    void serialize(
      Archive& ar, unordered_map<K, T, H, P, A>& m, unsigned int version)
    {
      detail::serialize_fca_container(ar, m, version);
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
    unordered_map(InputIterator, InputIterator,
      std::size_t = boost::unordered::detail::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> unordered_map<boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>, Hash, Pred,
        Allocator>;

    template <class Key, class T,
      class Hash = boost::hash<std::remove_const_t<Key> >,
      class Pred = std::equal_to<std::remove_const_t<Key> >,
      class Allocator = std::allocator<std::pair<const Key, T> >,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_pred_v<Pred> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_map(std::initializer_list<std::pair<Key, T> >,
      std::size_t = boost::unordered::detail::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> unordered_map<std::remove_const_t<Key>, T, Hash, Pred, Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_map(InputIterator, InputIterator, std::size_t, Allocator)
      -> unordered_map<boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>,
        boost::hash<boost::unordered::detail::iter_key_t<InputIterator> >,
        std::equal_to<boost::unordered::detail::iter_key_t<InputIterator> >,
        Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_map(InputIterator, InputIterator, Allocator)
      -> unordered_map<boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>,
        boost::hash<boost::unordered::detail::iter_key_t<InputIterator> >,
        std::equal_to<boost::unordered::detail::iter_key_t<InputIterator> >,
        Allocator>;

    template <class InputIterator, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_map(InputIterator, InputIterator, std::size_t, Hash, Allocator)
      -> unordered_map<boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>, Hash,
        std::equal_to<boost::unordered::detail::iter_key_t<InputIterator> >,
        Allocator>;

    template <class Key, class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_map(std::initializer_list<std::pair<Key, T> >, std::size_t,
      Allocator) -> unordered_map<std::remove_const_t<Key>, T,
      boost::hash<std::remove_const_t<Key> >,
      std::equal_to<std::remove_const_t<Key> >, Allocator>;

    template <class Key, class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_map(std::initializer_list<std::pair<Key, T> >, Allocator)
      -> unordered_map<std::remove_const_t<Key>, T,
        boost::hash<std::remove_const_t<Key> >,
        std::equal_to<std::remove_const_t<Key> >, Allocator>;

    template <class Key, class T, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_map(std::initializer_list<std::pair<Key, T> >, std::size_t, Hash,
      Allocator) -> unordered_map<std::remove_const_t<Key>, T, Hash,
      std::equal_to<std::remove_const_t<Key> >, Allocator>;

#endif

    template <class K, class T, class H, class P, class A>
    class unordered_multimap
    {
      template <typename, typename, typename, typename, typename>
      friend class unordered_map;

    public:
      typedef K key_type;
      typedef T mapped_type;
      typedef std::pair<const K, T> value_type;
      typedef typename boost::unordered::detail::type_identity<H>::type hasher;
      typedef
        typename boost::unordered::detail::type_identity<P>::type key_equal;
      typedef typename boost::unordered::detail::type_identity<A>::type
        allocator_type;

    private:
      typedef boost::unordered::detail::map<A, K, T, H, P> types;
      typedef typename types::value_allocator_traits value_allocator_traits;
      typedef typename types::table table;

    public:
      typedef typename value_allocator_traits::pointer pointer;
      typedef typename value_allocator_traits::const_pointer const_pointer;

      typedef value_type& reference;
      typedef value_type const& const_reference;

      typedef std::size_t size_type;
      typedef std::ptrdiff_t difference_type;

      typedef typename table::iterator iterator;
      typedef typename table::c_iterator const_iterator;
      typedef typename table::l_iterator local_iterator;
      typedef typename table::cl_iterator const_local_iterator;
      typedef typename types::node_type node_type;

    private:
      table table_;

    public:
      // constructors

      unordered_multimap();

      explicit unordered_multimap(size_type, const hasher& = hasher(),
        const key_equal& = key_equal(),
        const allocator_type& = allocator_type());

      template <class InputIt>
      unordered_multimap(InputIt, InputIt,
        size_type = boost::unordered::detail::default_bucket_count,
        const hasher& = hasher(), const key_equal& = key_equal(),
        const allocator_type& = allocator_type());

      unordered_multimap(unordered_multimap const&);

      unordered_multimap(unordered_multimap&& other)
        noexcept(table::nothrow_move_constructible)
          : table_(other.table_, boost::unordered::detail::move_tag())
      {
        // The move is done in table_
      }

      explicit unordered_multimap(allocator_type const&);

      unordered_multimap(unordered_multimap const&, allocator_type const&);

      unordered_multimap(unordered_multimap&&, allocator_type const&);

      unordered_multimap(std::initializer_list<value_type>,
        size_type = boost::unordered::detail::default_bucket_count,
        const hasher& = hasher(), const key_equal& l = key_equal(),
        const allocator_type& = allocator_type());

      explicit unordered_multimap(size_type, const allocator_type&);

      explicit unordered_multimap(
        size_type, const hasher&, const allocator_type&);

      template <class InputIterator>
      unordered_multimap(InputIterator, InputIterator, const allocator_type&);

      template <class InputIt>
      unordered_multimap(InputIt, InputIt, size_type, const allocator_type&);

      template <class InputIt>
      unordered_multimap(
        InputIt, InputIt, size_type, const hasher&, const allocator_type&);

      unordered_multimap(
        std::initializer_list<value_type>, const allocator_type&);

      unordered_multimap(
        std::initializer_list<value_type>, size_type, const allocator_type&);

      unordered_multimap(std::initializer_list<value_type>, size_type,
        const hasher&, const allocator_type&);

      // Destructor

      ~unordered_multimap() noexcept;

      // Assign

      unordered_multimap& operator=(unordered_multimap const& x)
      {
        table_.assign(x.table_, std::false_type());
        return *this;
      }

      unordered_multimap& operator=(unordered_multimap&& x)
        noexcept(value_allocator_traits::is_always_equal::value&&
            std::is_nothrow_move_assignable<H>::value&&
              std::is_nothrow_move_assignable<P>::value)
      {
        table_.move_assign(x.table_, std::false_type());
        return *this;
      }

      unordered_multimap& operator=(std::initializer_list<value_type>);

      allocator_type get_allocator() const noexcept
      {
        return allocator_type(table_.node_alloc());
      }

      // iterators

      iterator begin() noexcept { return iterator(table_.begin()); }

      const_iterator begin() const noexcept
      {
        return const_iterator(table_.begin());
      }

      iterator end() noexcept { return iterator(); }

      const_iterator end() const noexcept { return const_iterator(); }

      const_iterator cbegin() const noexcept
      {
        return const_iterator(table_.begin());
      }

      const_iterator cend() const noexcept { return const_iterator(); }

      // size and capacity

      BOOST_ATTRIBUTE_NODISCARD bool empty() const noexcept
      {
        return table_.size_ == 0;
      }

      size_type size() const noexcept { return table_.size_; }

      size_type max_size() const noexcept;

      // emplace

      template <class... Args> iterator emplace(Args&&... args)
      {
        return iterator(table_.emplace_equiv(
          boost::unordered::detail::func::construct_node_from_args(
            table_.node_alloc(), std::forward<Args>(args)...)));
      }

      template <class... Args>
      iterator emplace_hint(const_iterator hint, Args&&... args)
      {
        return iterator(table_.emplace_hint_equiv(
          hint, boost::unordered::detail::func::construct_node_from_args(
                  table_.node_alloc(), std::forward<Args>(args)...)));
      }

      iterator insert(value_type const& x) { return this->emplace(x); }

      iterator insert(value_type&& x) { return this->emplace(std::move(x)); }

      template <class P2>
      typename boost::enable_if<std::is_constructible<value_type, P2&&>,
        iterator>::type
      insert(P2&& obj)
      {
        return this->emplace(std::forward<P2>(obj));
      }

      iterator insert(const_iterator hint, value_type const& x)
      {
        return this->emplace_hint(hint, x);
      }

      iterator insert(const_iterator hint, value_type&& x)
      {
        return this->emplace_hint(hint, std::move(x));
      }

      template <class P2>
      typename boost::enable_if<std::is_constructible<value_type, P2&&>,
        iterator>::type
      insert(const_iterator hint, P2&& obj)
      {
        return this->emplace_hint(hint, std::forward<P2>(obj));
      }

      template <class InputIt> void insert(InputIt, InputIt);

      void insert(std::initializer_list<value_type>);

      // extract

      node_type extract(const_iterator position)
      {
        return node_type(
          table_.extract_by_iterator_equiv(position), table_.node_alloc());
      }

      node_type extract(const key_type& k)
      {
        return node_type(table_.extract_by_key_impl(k), table_.node_alloc());
      }

      template <class Key>
      typename boost::enable_if_c<
        detail::transparent_non_iterable<Key, unordered_multimap>::value,
        node_type>::type
      extract(const Key& k)
      {
        return node_type(table_.extract_by_key_impl(k), table_.node_alloc());
      }

      iterator insert(node_type&& np)
      {
        return table_.move_insert_node_type_equiv(np);
      }

      iterator insert(const_iterator hint, node_type&& np)
      {
        return table_.move_insert_node_type_with_hint_equiv(hint, np);
      }

      iterator erase(iterator);
      iterator erase(const_iterator);
      size_type erase(const key_type&);
      iterator erase(const_iterator, const_iterator);

      template <class Key>
      typename boost::enable_if_c<
        detail::transparent_non_iterable<Key, unordered_multimap>::value,
        size_type>::type
      erase(Key&& k)
      {
        return table_.erase_key_equiv_impl(std::forward<Key>(k));
      }

      BOOST_UNORDERED_DEPRECATED("Use erase instead")
      void quick_erase(const_iterator it) { erase(it); }
      BOOST_UNORDERED_DEPRECATED("Use erase instead")
      void erase_return_void(const_iterator it) { erase(it); }

      void swap(unordered_multimap&)
        noexcept(value_allocator_traits::is_always_equal::value&&
            boost::unordered::detail::is_nothrow_swappable<H>::value&&
              boost::unordered::detail::is_nothrow_swappable<P>::value);
      void clear() noexcept { table_.clear_impl(); }

      template <typename H2, typename P2>
      void merge(boost::unordered_multimap<K, T, H2, P2, A>& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_multimap<K, T, H2, P2, A>&& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_map<K, T, H2, P2, A>& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_map<K, T, H2, P2, A>&& source);

      // observers

      hasher hash_function() const;
      key_equal key_eq() const;

      // lookup

      iterator find(const key_type&);
      const_iterator find(const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        iterator>::type
      find(const Key& key)
      {
        return table_.find(key);
      }

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        const_iterator>::type
      find(const Key& key) const
      {
        return const_iterator(table_.find(key));
      }

      template <class CompatibleKey, class CompatibleHash,
        class CompatiblePredicate>
      iterator find(CompatibleKey const&, CompatibleHash const&,
        CompatiblePredicate const&);

      template <class CompatibleKey, class CompatibleHash,
        class CompatiblePredicate>
      const_iterator find(CompatibleKey const&, CompatibleHash const&,
        CompatiblePredicate const&) const;

      bool contains(key_type const& k) const
      {
        return table_.find(k) != this->end();
      }

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        bool>::type
      contains(const Key& k) const
      {
        return table_.find(k) != this->end();
      }

      size_type count(const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        size_type>::type
      count(const Key& k) const
      {
        return table_.group_count(k);
      }

      std::pair<iterator, iterator> equal_range(const key_type&);
      std::pair<const_iterator, const_iterator> equal_range(
        const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        std::pair<iterator, iterator> >::type
      equal_range(const Key& key)
      {
        iterator p = table_.find(key);
        return std::make_pair(p, table_.next_group(key, p));
      }

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        std::pair<const_iterator, const_iterator> >::type
      equal_range(const Key& key) const
      {
        iterator p = table_.find(key);
        return std::make_pair(
          const_iterator(p), const_iterator(table_.next_group(key, p)));
      }

      // bucket interface

      size_type bucket_count() const noexcept { return table_.bucket_count(); }

      size_type max_bucket_count() const noexcept
      {
        return table_.max_bucket_count();
      }

      size_type bucket_size(size_type) const;

      size_type bucket(const key_type& k) const
      {
        return table_.hash_to_bucket(table_.hash(k));
      }

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        size_type>::type
      bucket(Key&& k) const
      {
        return table_.hash_to_bucket(table_.hash(std::forward<Key>(k)));
      }

      local_iterator begin(size_type n)
      {
        return local_iterator(table_.begin(n));
      }

      const_local_iterator begin(size_type n) const
      {
        return const_local_iterator(table_.begin(n));
      }

      local_iterator end(size_type) { return local_iterator(); }

      const_local_iterator end(size_type) const
      {
        return const_local_iterator();
      }

      const_local_iterator cbegin(size_type n) const
      {
        return const_local_iterator(table_.begin(n));
      }

      const_local_iterator cend(size_type) const
      {
        return const_local_iterator();
      }

      // hash policy

      float load_factor() const noexcept;
      float max_load_factor() const noexcept { return table_.mlf_; }
      void max_load_factor(float) noexcept;
      void rehash(size_type);
      void reserve(size_type);

#if !BOOST_WORKAROUND(BOOST_BORLANDC, < 0x0582)
      friend bool operator==
        <K, T, H, P, A>(unordered_multimap const&, unordered_multimap const&);
      friend bool operator!=
        <K, T, H, P, A>(unordered_multimap const&, unordered_multimap const&);
#endif
    }; // class template unordered_multimap

    template <class Archive, class K, class T, class H, class P, class A>
    void serialize(
      Archive& ar, unordered_multimap<K, T, H, P, A>& m, unsigned int version)
    {
      detail::serialize_fca_container(ar, m, version);
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
    unordered_multimap(InputIterator, InputIterator,
      std::size_t = boost::unordered::detail::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> unordered_multimap<boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>, Hash, Pred,
        Allocator>;

    template <class Key, class T,
      class Hash = boost::hash<std::remove_const_t<Key> >,
      class Pred = std::equal_to<std::remove_const_t<Key> >,
      class Allocator = std::allocator<std::pair<const Key, T> >,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_pred_v<Pred> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multimap(std::initializer_list<std::pair<Key, T> >,
      std::size_t = boost::unordered::detail::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> unordered_multimap<std::remove_const_t<Key>, T, Hash, Pred, Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multimap(InputIterator, InputIterator, std::size_t, Allocator)
      -> unordered_multimap<boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>,
        boost::hash<boost::unordered::detail::iter_key_t<InputIterator> >,
        std::equal_to<boost::unordered::detail::iter_key_t<InputIterator> >,
        Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multimap(InputIterator, InputIterator, Allocator)
      -> unordered_multimap<boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>,
        boost::hash<boost::unordered::detail::iter_key_t<InputIterator> >,
        std::equal_to<boost::unordered::detail::iter_key_t<InputIterator> >,
        Allocator>;

    template <class InputIterator, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multimap(
      InputIterator, InputIterator, std::size_t, Hash, Allocator)
      -> unordered_multimap<boost::unordered::detail::iter_key_t<InputIterator>,
        boost::unordered::detail::iter_val_t<InputIterator>, Hash,
        std::equal_to<boost::unordered::detail::iter_key_t<InputIterator> >,
        Allocator>;

    template <class Key, class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multimap(std::initializer_list<std::pair<Key, T> >, std::size_t,
      Allocator) -> unordered_multimap<std::remove_const_t<Key>, T,
      boost::hash<std::remove_const_t<Key> >,
      std::equal_to<std::remove_const_t<Key> >, Allocator>;

    template <class Key, class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multimap(std::initializer_list<std::pair<Key, T> >, Allocator)
      -> unordered_multimap<std::remove_const_t<Key>, T,
        boost::hash<std::remove_const_t<Key> >,
        std::equal_to<std::remove_const_t<Key> >, Allocator>;

    template <class Key, class T, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multimap(std::initializer_list<std::pair<Key, T> >, std::size_t,
      Hash, Allocator) -> unordered_multimap<std::remove_const_t<Key>, T, Hash,
      std::equal_to<std::remove_const_t<Key> >, Allocator>;

#endif

    ////////////////////////////////////////////////////////////////////////////

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map()
    {
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(size_type n, const hasher& hf,
      const key_equal& eql, const allocator_type& a)
        : table_(n, hf, eql, a)
    {
    }

    template <class K, class T, class H, class P, class A>
    template <class InputIt>
    unordered_map<K, T, H, P, A>::unordered_map(InputIt f, InputIt l,
      size_type n, const hasher& hf, const key_equal& eql,
      const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(f, l, n), hf, eql, a)
    {
      this->insert(f, l);
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(unordered_map const& other)
        : table_(other.table_,
            unordered_map::value_allocator_traits::
              select_on_container_copy_construction(other.get_allocator()))
    {
      if (other.size()) {
        table_.copy_buckets(other.table_, std::true_type());
      }
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(allocator_type const& a)
        : table_(boost::unordered::detail::default_bucket_count, hasher(),
            key_equal(), a)
    {
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(
      unordered_map const& other, allocator_type const& a)
        : table_(other.table_, a)
    {
      if (other.table_.size_) {
        table_.copy_buckets(other.table_, std::true_type());
      }
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(
      unordered_map&& other, allocator_type const& a)
        : table_(other.table_, a, boost::unordered::detail::move_tag())
    {
      table_.move_construct_buckets(other.table_);
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(
      std::initializer_list<value_type> list, size_type n, const hasher& hf,
      const key_equal& eql, const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hf, eql, a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(
      size_type n, const allocator_type& a)
        : table_(n, hasher(), key_equal(), a)
    {
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(
      size_type n, const hasher& hf, const allocator_type& a)
        : table_(n, hf, key_equal(), a)
    {
    }

    template <class K, class T, class H, class P, class A>
    template <class InputIterator>
    unordered_map<K, T, H, P, A>::unordered_map(
      InputIterator f, InputIterator l, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(
                   f, l, detail::default_bucket_count),
            hasher(), key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class K, class T, class H, class P, class A>
    template <class InputIt>
    unordered_map<K, T, H, P, A>::unordered_map(
      InputIt f, InputIt l, size_type n, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(f, l, n), hasher(),
            key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class K, class T, class H, class P, class A>
    template <class InputIt>
    unordered_map<K, T, H, P, A>::unordered_map(InputIt f, InputIt l,
      size_type n, const hasher& hf, const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(f, l, n), hf, key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(
      std::initializer_list<value_type> list, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(
                   list.begin(), list.end(), detail::default_bucket_count),
            hasher(), key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(
      std::initializer_list<value_type> list, size_type n,
      const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hasher(), key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::unordered_map(
      std::initializer_list<value_type> list, size_type n, const hasher& hf,
      const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hf, key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>::~unordered_map() noexcept
    {
    }

    template <class K, class T, class H, class P, class A>
    unordered_map<K, T, H, P, A>& unordered_map<K, T, H, P, A>::operator=(
      std::initializer_list<value_type> list)
    {
      this->clear();
      this->insert(list.begin(), list.end());
      return *this;
    }

    // size and capacity

    template <class K, class T, class H, class P, class A>
    std::size_t unordered_map<K, T, H, P, A>::max_size() const noexcept
    {
      using namespace std;

      // size <= mlf_ * count
      return boost::unordered::detail::double_to_size(
               ceil(static_cast<double>(table_.mlf_) *
                    static_cast<double>(table_.max_bucket_count()))) -
             1;
    }

    // modifiers

    template <class K, class T, class H, class P, class A>
    template <class InputIt>
    void unordered_map<K, T, H, P, A>::insert(InputIt first, InputIt last)
    {
      if (first != last) {
        table_.insert_range_unique(
          table::extractor::extract(*first), first, last);
      }
    }

    template <class K, class T, class H, class P, class A>
    void unordered_map<K, T, H, P, A>::insert(
      std::initializer_list<value_type> list)
    {
      this->insert(list.begin(), list.end());
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::iterator
    unordered_map<K, T, H, P, A>::erase(iterator position)
    {
      return table_.erase_node(position);
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::iterator
    unordered_map<K, T, H, P, A>::erase(const_iterator position)
    {
      return table_.erase_node(position);
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::size_type
    unordered_map<K, T, H, P, A>::erase(const key_type& k)
    {
      return table_.erase_key_unique_impl(k);
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::iterator
    unordered_map<K, T, H, P, A>::erase(
      const_iterator first, const_iterator last)
    {
      return table_.erase_nodes_range(first, last);
    }

    template <class K, class T, class H, class P, class A>
    void unordered_map<K, T, H, P, A>::swap(unordered_map& other)
      noexcept(value_allocator_traits::is_always_equal::value&&
          boost::unordered::detail::is_nothrow_swappable<H>::value&&
            boost::unordered::detail::is_nothrow_swappable<P>::value)
    {
      table_.swap(other.table_);
    }

    template <class K, class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_map<K, T, H, P, A>::merge(
      boost::unordered_map<K, T, H2, P2, A>& source)
    {
      table_.merge_unique(source.table_);
    }

    template <class K, class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_map<K, T, H, P, A>::merge(
      boost::unordered_map<K, T, H2, P2, A>&& source)
    {
      table_.merge_unique(source.table_);
    }

    template <class K, class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_map<K, T, H, P, A>::merge(
      boost::unordered_multimap<K, T, H2, P2, A>& source)
    {
      table_.merge_unique(source.table_);
    }

    template <class K, class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_map<K, T, H, P, A>::merge(
      boost::unordered_multimap<K, T, H2, P2, A>&& source)
    {
      table_.merge_unique(source.table_);
    }

    // observers

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::hasher
    unordered_map<K, T, H, P, A>::hash_function() const
    {
      return table_.hash_function();
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::key_equal
    unordered_map<K, T, H, P, A>::key_eq() const
    {
      return table_.key_eq();
    }

    // lookup

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::iterator
    unordered_map<K, T, H, P, A>::find(const key_type& k)
    {
      return iterator(table_.find(k));
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::const_iterator
    unordered_map<K, T, H, P, A>::find(const key_type& k) const
    {
      return const_iterator(table_.find(k));
    }

    template <class K, class T, class H, class P, class A>
    template <class CompatibleKey, class CompatibleHash,
      class CompatiblePredicate>
    typename unordered_map<K, T, H, P, A>::iterator
    unordered_map<K, T, H, P, A>::find(CompatibleKey const& k,
      CompatibleHash const& hash, CompatiblePredicate const& eq)
    {
      return table_.transparent_find(k, hash, eq);
    }

    template <class K, class T, class H, class P, class A>
    template <class CompatibleKey, class CompatibleHash,
      class CompatiblePredicate>
    typename unordered_map<K, T, H, P, A>::const_iterator
    unordered_map<K, T, H, P, A>::find(CompatibleKey const& k,
      CompatibleHash const& hash, CompatiblePredicate const& eq) const
    {
      return table_.transparent_find(k, hash, eq);
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::size_type
    unordered_map<K, T, H, P, A>::count(const key_type& k) const
    {
      return table_.find_node(k) ? 1 : 0;
    }

    template <class K, class T, class H, class P, class A>
    std::pair<typename unordered_map<K, T, H, P, A>::iterator,
      typename unordered_map<K, T, H, P, A>::iterator>
    unordered_map<K, T, H, P, A>::equal_range(const key_type& k)
    {
      iterator first = table_.find(k);
      iterator second = first;
      if (second != this->end()) {
        ++second;
      }
      return std::make_pair(first, second);
    }

    template <class K, class T, class H, class P, class A>
    std::pair<typename unordered_map<K, T, H, P, A>::const_iterator,
      typename unordered_map<K, T, H, P, A>::const_iterator>
    unordered_map<K, T, H, P, A>::equal_range(const key_type& k) const
    {
      iterator first = table_.find(k);
      iterator second = first;
      if (second != this->end()) {
        ++second;
      }
      return std::make_pair(const_iterator(first), const_iterator(second));
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::mapped_type&
    unordered_map<K, T, H, P, A>::operator[](const key_type& k)
    {
      return table_.try_emplace_unique(k).first->second;
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::mapped_type&
    unordered_map<K, T, H, P, A>::operator[](key_type&& k)
    {
      return table_.try_emplace_unique(std::move(k)).first->second;
    }

    template <class K, class T, class H, class P, class A>
    template <class Key>
    typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
      typename unordered_map<K, T, H, P, A>::mapped_type&>::type
    unordered_map<K, T, H, P, A>::operator[](Key&& k)
    {
      return table_.try_emplace_unique(std::forward<Key>(k)).first->second;
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::mapped_type&
    unordered_map<K, T, H, P, A>::at(const key_type& k)
    {
      typedef typename table::node_pointer node_pointer;

      if (table_.size_) {
        node_pointer p = table_.find_node(k);
        if (p)
          return p->value().second;
      }

      boost::unordered::detail::throw_out_of_range(
        "Unable to find key in unordered_map.");
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::mapped_type const&
    unordered_map<K, T, H, P, A>::at(const key_type& k) const
    {
      typedef typename table::node_pointer node_pointer;

      if (table_.size_) {
        node_pointer p = table_.find_node(k);
        if (p)
          return p->value().second;
      }

      boost::unordered::detail::throw_out_of_range(
        "Unable to find key in unordered_map.");
    }

    template <class K, class T, class H, class P, class A>
    template <class Key>
    typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
      typename unordered_map<K, T, H, P, A>::mapped_type&>::type
    unordered_map<K, T, H, P, A>::at(Key&& k)
    {
      typedef typename table::node_pointer node_pointer;

      if (table_.size_) {
        node_pointer p = table_.find_node(std::forward<Key>(k));
        if (p)
          return p->value().second;
      }

      boost::unordered::detail::throw_out_of_range(
        "Unable to find key in unordered_map.");
    }

    template <class K, class T, class H, class P, class A>
    template <class Key>
    typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
      typename unordered_map<K, T, H, P, A>::mapped_type const&>::type
    unordered_map<K, T, H, P, A>::at(Key&& k) const
    {
      typedef typename table::node_pointer node_pointer;

      if (table_.size_) {
        node_pointer p = table_.find_node(std::forward<Key>(k));
        if (p)
          return p->value().second;
      }

      boost::unordered::detail::throw_out_of_range(
        "Unable to find key in unordered_map.");
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_map<K, T, H, P, A>::size_type
    unordered_map<K, T, H, P, A>::bucket_size(size_type n) const
    {
      return table_.bucket_size(n);
    }

    // hash policy

    template <class K, class T, class H, class P, class A>
    float unordered_map<K, T, H, P, A>::load_factor() const noexcept
    {
      if (table_.size_ == 0) {
        return 0.0f;
      }

      BOOST_ASSERT(table_.bucket_count() != 0);
      return static_cast<float>(table_.size_) /
             static_cast<float>(table_.bucket_count());
    }

    template <class K, class T, class H, class P, class A>
    void unordered_map<K, T, H, P, A>::max_load_factor(float m) noexcept
    {
      table_.max_load_factor(m);
    }

    template <class K, class T, class H, class P, class A>
    void unordered_map<K, T, H, P, A>::rehash(size_type n)
    {
      table_.rehash(n);
    }

    template <class K, class T, class H, class P, class A>
    void unordered_map<K, T, H, P, A>::reserve(size_type n)
    {
      table_.reserve(n);
    }

    template <class K, class T, class H, class P, class A>
    inline bool operator==(unordered_map<K, T, H, P, A> const& m1,
      unordered_map<K, T, H, P, A> const& m2)
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_map<K, T, H, P, A> x;
      };
#endif
      return m1.table_.equals_unique(m2.table_);
    }

    template <class K, class T, class H, class P, class A>
    inline bool operator!=(unordered_map<K, T, H, P, A> const& m1,
      unordered_map<K, T, H, P, A> const& m2)
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_map<K, T, H, P, A> x;
      };
#endif
      return !m1.table_.equals_unique(m2.table_);
    }

    template <class K, class T, class H, class P, class A>
    inline void swap(unordered_map<K, T, H, P, A>& m1,
      unordered_map<K, T, H, P, A>& m2) noexcept(noexcept(m1.swap(m2)))
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_map<K, T, H, P, A> x;
      };
#endif
      m1.swap(m2);
    }

    template <class K, class T, class H, class P, class A, class Predicate>
    typename unordered_map<K, T, H, P, A>::size_type erase_if(
      unordered_map<K, T, H, P, A>& c, Predicate pred)
    {
      return detail::erase_if(c, pred);
    }

    ////////////////////////////////////////////////////////////////////////////

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap()
    {
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(size_type n,
      const hasher& hf, const key_equal& eql, const allocator_type& a)
        : table_(n, hf, eql, a)
    {
    }

    template <class K, class T, class H, class P, class A>
    template <class InputIt>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(InputIt f, InputIt l,
      size_type n, const hasher& hf, const key_equal& eql,
      const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(f, l, n), hf, eql, a)
    {
      this->insert(f, l);
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      unordered_multimap const& other)
        : table_(other.table_,
            unordered_multimap::value_allocator_traits::
              select_on_container_copy_construction(other.get_allocator()))
    {
      if (other.table_.size_) {
        table_.copy_buckets(other.table_, std::false_type());
      }
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      allocator_type const& a)
        : table_(boost::unordered::detail::default_bucket_count, hasher(),
            key_equal(), a)
    {
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      unordered_multimap const& other, allocator_type const& a)
        : table_(other.table_, a)
    {
      if (other.table_.size_) {
        table_.copy_buckets(other.table_, std::false_type());
      }
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      unordered_multimap&& other, allocator_type const& a)
        : table_(other.table_, a, boost::unordered::detail::move_tag())
    {
      table_.move_construct_buckets(other.table_);
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      std::initializer_list<value_type> list, size_type n, const hasher& hf,
      const key_equal& eql, const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hf, eql, a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      size_type n, const allocator_type& a)
        : table_(n, hasher(), key_equal(), a)
    {
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      size_type n, const hasher& hf, const allocator_type& a)
        : table_(n, hf, key_equal(), a)
    {
    }

    template <class K, class T, class H, class P, class A>
    template <class InputIterator>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      InputIterator f, InputIterator l, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(
                   f, l, detail::default_bucket_count),
            hasher(), key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class K, class T, class H, class P, class A>
    template <class InputIt>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      InputIt f, InputIt l, size_type n, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(f, l, n), hasher(),
            key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class K, class T, class H, class P, class A>
    template <class InputIt>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(InputIt f, InputIt l,
      size_type n, const hasher& hf, const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(f, l, n), hf, key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      std::initializer_list<value_type> list, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(
                   list.begin(), list.end(), detail::default_bucket_count),
            hasher(), key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      std::initializer_list<value_type> list, size_type n,
      const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hasher(), key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::unordered_multimap(
      std::initializer_list<value_type> list, size_type n, const hasher& hf,
      const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hf, key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>::~unordered_multimap() noexcept
    {
    }

    template <class K, class T, class H, class P, class A>
    unordered_multimap<K, T, H, P, A>&
    unordered_multimap<K, T, H, P, A>::operator=(
      std::initializer_list<value_type> list)
    {
      this->clear();
      this->insert(list.begin(), list.end());
      return *this;
    }

    // size and capacity

    template <class K, class T, class H, class P, class A>
    std::size_t unordered_multimap<K, T, H, P, A>::max_size() const noexcept
    {
      using namespace std;

      // size <= mlf_ * count
      return boost::unordered::detail::double_to_size(
               ceil(static_cast<double>(table_.mlf_) *
                    static_cast<double>(table_.max_bucket_count()))) -
             1;
    }

    // modifiers

    template <class K, class T, class H, class P, class A>
    template <class InputIt>
    void unordered_multimap<K, T, H, P, A>::insert(InputIt first, InputIt last)
    {
      table_.insert_range_equiv(first, last);
    }

    template <class K, class T, class H, class P, class A>
    void unordered_multimap<K, T, H, P, A>::insert(
      std::initializer_list<value_type> list)
    {
      this->insert(list.begin(), list.end());
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_multimap<K, T, H, P, A>::iterator
    unordered_multimap<K, T, H, P, A>::erase(iterator position)
    {
      BOOST_ASSERT(position != this->end());
      return table_.erase_node(position);
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_multimap<K, T, H, P, A>::iterator
    unordered_multimap<K, T, H, P, A>::erase(const_iterator position)
    {
      BOOST_ASSERT(position != this->end());
      return table_.erase_node(position);
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_multimap<K, T, H, P, A>::size_type
    unordered_multimap<K, T, H, P, A>::erase(const key_type& k)
    {
      return table_.erase_key_equiv(k);
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_multimap<K, T, H, P, A>::iterator
    unordered_multimap<K, T, H, P, A>::erase(
      const_iterator first, const_iterator last)
    {
      return table_.erase_nodes_range(first, last);
    }

    template <class K, class T, class H, class P, class A>
    void unordered_multimap<K, T, H, P, A>::swap(unordered_multimap& other)
      noexcept(value_allocator_traits::is_always_equal::value&&
          boost::unordered::detail::is_nothrow_swappable<H>::value&&
            boost::unordered::detail::is_nothrow_swappable<P>::value)
    {
      table_.swap(other.table_);
    }

    // observers

    template <class K, class T, class H, class P, class A>
    typename unordered_multimap<K, T, H, P, A>::hasher
    unordered_multimap<K, T, H, P, A>::hash_function() const
    {
      return table_.hash_function();
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_multimap<K, T, H, P, A>::key_equal
    unordered_multimap<K, T, H, P, A>::key_eq() const
    {
      return table_.key_eq();
    }

    template <class K, class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_multimap<K, T, H, P, A>::merge(
      boost::unordered_multimap<K, T, H2, P2, A>& source)
    {
      while (!source.empty()) {
        insert(source.extract(source.begin()));
      }
    }

    template <class K, class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_multimap<K, T, H, P, A>::merge(
      boost::unordered_multimap<K, T, H2, P2, A>&& source)
    {
      while (!source.empty()) {
        insert(source.extract(source.begin()));
      }
    }

    template <class K, class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_multimap<K, T, H, P, A>::merge(
      boost::unordered_map<K, T, H2, P2, A>& source)
    {
      while (!source.empty()) {
        insert(source.extract(source.begin()));
      }
    }

    template <class K, class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_multimap<K, T, H, P, A>::merge(
      boost::unordered_map<K, T, H2, P2, A>&& source)
    {
      while (!source.empty()) {
        insert(source.extract(source.begin()));
      }
    }

    // lookup

    template <class K, class T, class H, class P, class A>
    typename unordered_multimap<K, T, H, P, A>::iterator
    unordered_multimap<K, T, H, P, A>::find(const key_type& k)
    {
      return iterator(table_.find(k));
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_multimap<K, T, H, P, A>::const_iterator
    unordered_multimap<K, T, H, P, A>::find(const key_type& k) const
    {
      return const_iterator(table_.find(k));
    }

    template <class K, class T, class H, class P, class A>
    template <class CompatibleKey, class CompatibleHash,
      class CompatiblePredicate>
    typename unordered_multimap<K, T, H, P, A>::iterator
    unordered_multimap<K, T, H, P, A>::find(CompatibleKey const& k,
      CompatibleHash const& hash, CompatiblePredicate const& eq)
    {
      return table_.transparent_find(k, hash, eq);
    }

    template <class K, class T, class H, class P, class A>
    template <class CompatibleKey, class CompatibleHash,
      class CompatiblePredicate>
    typename unordered_multimap<K, T, H, P, A>::const_iterator
    unordered_multimap<K, T, H, P, A>::find(CompatibleKey const& k,
      CompatibleHash const& hash, CompatiblePredicate const& eq) const
    {
      return table_.transparent_find(k, hash, eq);
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_multimap<K, T, H, P, A>::size_type
    unordered_multimap<K, T, H, P, A>::count(const key_type& k) const
    {
      return table_.group_count(k);
    }

    template <class K, class T, class H, class P, class A>
    std::pair<typename unordered_multimap<K, T, H, P, A>::iterator,
      typename unordered_multimap<K, T, H, P, A>::iterator>
    unordered_multimap<K, T, H, P, A>::equal_range(const key_type& k)
    {
      iterator n = table_.find(k);
      return std::make_pair(n, (n == end() ? n : table_.next_group(k, n)));
    }

    template <class K, class T, class H, class P, class A>
    std::pair<typename unordered_multimap<K, T, H, P, A>::const_iterator,
      typename unordered_multimap<K, T, H, P, A>::const_iterator>
    unordered_multimap<K, T, H, P, A>::equal_range(const key_type& k) const
    {
      iterator n = table_.find(k);
      return std::make_pair(const_iterator(n),
        const_iterator(n == end() ? n : table_.next_group(k, n)));
    }

    template <class K, class T, class H, class P, class A>
    typename unordered_multimap<K, T, H, P, A>::size_type
    unordered_multimap<K, T, H, P, A>::bucket_size(size_type n) const
    {
      return table_.bucket_size(n);
    }

    // hash policy

    template <class K, class T, class H, class P, class A>
    float unordered_multimap<K, T, H, P, A>::load_factor() const noexcept
    {
      if (table_.size_ == 0) {
        return 0.0f;
      }

      BOOST_ASSERT(table_.bucket_count() != 0);
      return static_cast<float>(table_.size_) /
             static_cast<float>(table_.bucket_count());
    }

    template <class K, class T, class H, class P, class A>
    void unordered_multimap<K, T, H, P, A>::max_load_factor(float m) noexcept
    {
      table_.max_load_factor(m);
    }

    template <class K, class T, class H, class P, class A>
    void unordered_multimap<K, T, H, P, A>::rehash(size_type n)
    {
      table_.rehash(n);
    }

    template <class K, class T, class H, class P, class A>
    void unordered_multimap<K, T, H, P, A>::reserve(size_type n)
    {
      table_.reserve(n);
    }

    template <class K, class T, class H, class P, class A>
    inline bool operator==(unordered_multimap<K, T, H, P, A> const& m1,
      unordered_multimap<K, T, H, P, A> const& m2)
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_multimap<K, T, H, P, A> x;
      };
#endif
      return m1.table_.equals_equiv(m2.table_);
    }

    template <class K, class T, class H, class P, class A>
    inline bool operator!=(unordered_multimap<K, T, H, P, A> const& m1,
      unordered_multimap<K, T, H, P, A> const& m2)
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_multimap<K, T, H, P, A> x;
      };
#endif
      return !m1.table_.equals_equiv(m2.table_);
    }

    template <class K, class T, class H, class P, class A>
    inline void swap(unordered_multimap<K, T, H, P, A>& m1,
      unordered_multimap<K, T, H, P, A>& m2) noexcept(noexcept(m1.swap(m2)))
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_multimap<K, T, H, P, A> x;
      };
#endif
      m1.swap(m2);
    }

    template <class K, class T, class H, class P, class A, class Predicate>
    typename unordered_multimap<K, T, H, P, A>::size_type erase_if(
      unordered_multimap<K, T, H, P, A>& c, Predicate pred)
    {
      return detail::erase_if(c, pred);
    }

    template <typename N, class K, class T, class A> class node_handle_map
    {
      template <typename Types> friend struct ::boost::unordered::detail::table;
      template <class K2, class T2, class H2, class P2, class A2>
      friend class boost::unordered::unordered_map;
      template <class K2, class T2, class H2, class P2, class A2>
      friend class boost::unordered::unordered_multimap;

      typedef typename boost::allocator_rebind<A, std::pair<K const, T> >::type
        value_allocator;

      typedef N node;
      typedef typename boost::allocator_rebind<A, node>::type node_allocator;

      typedef
        typename boost::allocator_pointer<node_allocator>::type node_pointer;

    public:
      typedef K key_type;
      typedef T mapped_type;
      typedef A allocator_type;

    private:
      node_pointer ptr_;
      boost::unordered::detail::optional<value_allocator> alloc_;

      node_handle_map(node_pointer ptr, allocator_type const& a)
          : ptr_(ptr), alloc_(a)
      {
      }

    public:
      constexpr node_handle_map() noexcept : ptr_(), alloc_() {}
      node_handle_map(node_handle_map const&) = delete;
      node_handle_map& operator=(node_handle_map const&) = delete;

      ~node_handle_map()
      {
        if (ptr_) {
          node_allocator node_alloc(*alloc_);
          boost::unordered::detail::node_tmp<node_allocator> tmp(
            ptr_, node_alloc);
        }
      }

      node_handle_map(node_handle_map&& n) noexcept
          : ptr_(n.ptr_),
            alloc_(std::move(n.alloc_))
      {
        n.ptr_ = node_pointer();
      }

      node_handle_map& operator=(node_handle_map&& n)
      {
        BOOST_ASSERT(!alloc_.has_value() ||
                     boost::allocator_propagate_on_container_move_assignment<
                       value_allocator>::type::value ||
                     (n.alloc_.has_value() && alloc_ == n.alloc_));

        if (ptr_) {
          node_allocator node_alloc(*alloc_);
          boost::unordered::detail::node_tmp<node_allocator> tmp(
            ptr_, node_alloc);
          ptr_ = node_pointer();
        }

        if (!alloc_.has_value() ||
            boost::allocator_propagate_on_container_move_assignment<
              value_allocator>::type::value) {
          alloc_ = std::move(n.alloc_);
        }
        ptr_ = n.ptr_;
        n.ptr_ = node_pointer();

        return *this;
      }

      key_type& key() const
      {
        return const_cast<key_type&>(ptr_->value().first);
      }

      mapped_type& mapped() const { return ptr_->value().second; }

      allocator_type get_allocator() const { return *alloc_; }

      explicit operator bool() const noexcept
      {
        return !this->operator!();
      }

      bool operator!() const noexcept { return ptr_ ? 0 : 1; }

      BOOST_ATTRIBUTE_NODISCARD bool empty() const noexcept
      {
        return ptr_ ? 0 : 1;
      }

      void swap(node_handle_map& n)
        noexcept(boost::allocator_propagate_on_container_swap<
                   value_allocator>::type::value ||
                 boost::allocator_is_always_equal<value_allocator>::type::value)
      {

        BOOST_ASSERT(!alloc_.has_value() || !n.alloc_.has_value() ||
                     boost::allocator_propagate_on_container_swap<
                       value_allocator>::type::value ||
                     alloc_ == n.alloc_);
        if (boost::allocator_propagate_on_container_swap<
              value_allocator>::type::value ||
            !alloc_.has_value() || !n.alloc_.has_value()) {
          boost::core::invoke_swap(alloc_, n.alloc_);
        }
        boost::core::invoke_swap(ptr_, n.ptr_);
      }
    };

    template <class N, class K, class T, class A>
    void swap(node_handle_map<N, K, T, A>& x, node_handle_map<N, K, T, A>& y)
      noexcept(noexcept(x.swap(y)))
    {
      x.swap(y);
    }

    template <class Iter, class NodeType> struct insert_return_type_map
    {
    public:
      Iter position;
      bool inserted;
      NodeType node;

      insert_return_type_map() : position(), inserted(false), node() {}
      insert_return_type_map(insert_return_type_map const&) = delete;
      insert_return_type_map& operator=(insert_return_type_map const&) = delete;

      insert_return_type_map(insert_return_type_map&& x) noexcept
          : position(x.position),
            inserted(x.inserted),
            node(std::move(x.node))
      {
      }

      insert_return_type_map& operator=(insert_return_type_map&& x)
      {
        inserted = x.inserted;
        position = x.position;
        node = std::move(x.node);
        return *this;
      }
    };

    template <class Iter, class NodeType>
    void swap(insert_return_type_map<Iter, NodeType>& x,
      insert_return_type_map<Iter, NodeType>& y)
    {
      boost::core::invoke_swap(x.node, y.node);
      boost::core::invoke_swap(x.inserted, y.inserted);
      boost::core::invoke_swap(x.position, y.position);
    }
  } // namespace unordered

  namespace serialization {
    template <class K, class T, class H, class P, class A>
    struct version<boost::unordered_map<K, T, H, P, A> >
    {
      BOOST_STATIC_CONSTANT(int, value = 1);
    };

    template <class K, class T, class H, class P, class A>
    struct version<boost::unordered_multimap<K, T, H, P, A> >
    {
      BOOST_STATIC_CONSTANT(int, value = 1);
    };
  } // namespace serialization

} // namespace boost

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

#endif // BOOST_UNORDERED_UNORDERED_MAP_HPP_INCLUDED
