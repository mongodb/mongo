// Copyright (C) 2003-2004 Jeremy B. Maitin-Shepard.
// Copyright (C) 2005-2011 Daniel James.
// Copyright (C) 2022-2023 Christian Mazakas
// Copyright (C) 2024 Joaquin M Lopez Munoz.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/unordered for documentation

#ifndef BOOST_UNORDERED_UNORDERED_SET_HPP_INCLUDED
#define BOOST_UNORDERED_UNORDERED_SET_HPP_INCLUDED

#include <boost/config.hpp>
#if defined(BOOST_HAS_PRAGMA_ONCE)
#pragma once
#endif

#include <boost/unordered/detail/serialize_fca_container.hpp>
#include <boost/unordered/detail/set.hpp>
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
    template <class T, class H, class P, class A> class unordered_set
    {
      template <typename, typename, typename, typename>
      friend class unordered_multiset;

    public:
      typedef T key_type;
      typedef T value_type;
      typedef H hasher;
      typedef P key_equal;
      typedef A allocator_type;

    private:
      typedef boost::unordered::detail::set<A, T, H, P> types;
      typedef typename types::value_allocator_traits value_allocator_traits;
      typedef typename types::table table;

    public:
      typedef typename value_allocator_traits::pointer pointer;
      typedef typename value_allocator_traits::const_pointer const_pointer;

      typedef value_type& reference;
      typedef value_type const& const_reference;

      typedef std::size_t size_type;
      typedef std::ptrdiff_t difference_type;

      typedef typename table::c_iterator iterator;
      typedef typename table::c_iterator const_iterator;
      typedef typename table::cl_iterator local_iterator;
      typedef typename table::cl_iterator const_local_iterator;
      typedef typename types::node_type node_type;
      typedef typename types::insert_return_type insert_return_type;

    private:
      table table_;

    public:
      // constructors

      unordered_set();

      explicit unordered_set(size_type, const hasher& = hasher(),
        const key_equal& = key_equal(),
        const allocator_type& = allocator_type());

      template <class InputIt>
      unordered_set(InputIt, InputIt,
        size_type = boost::unordered::detail::default_bucket_count,
        const hasher& = hasher(), const key_equal& = key_equal(),
        const allocator_type& = allocator_type());

      unordered_set(unordered_set const&);

      unordered_set(unordered_set&& other)
        noexcept(table::nothrow_move_constructible)
          : table_(other.table_, boost::unordered::detail::move_tag())
      {
        // The move is done in table_
      }

      explicit unordered_set(allocator_type const&);

      unordered_set(unordered_set const&, allocator_type const&);

      unordered_set(unordered_set&&, allocator_type const&);

      unordered_set(std::initializer_list<value_type>,
        size_type = boost::unordered::detail::default_bucket_count,
        const hasher& = hasher(), const key_equal& l = key_equal(),
        const allocator_type& = allocator_type());

      explicit unordered_set(size_type, const allocator_type&);

      explicit unordered_set(size_type, const hasher&, const allocator_type&);

      template <class InputIterator>
      unordered_set(InputIterator, InputIterator, const allocator_type&);

      template <class InputIt>
      unordered_set(InputIt, InputIt, size_type, const allocator_type&);

      template <class InputIt>
      unordered_set(
        InputIt, InputIt, size_type, const hasher&, const allocator_type&);

      unordered_set(std::initializer_list<value_type>, const allocator_type&);

      unordered_set(
        std::initializer_list<value_type>, size_type, const allocator_type&);

      unordered_set(std::initializer_list<value_type>, size_type, const hasher&,
        const allocator_type&);

      // Destructor

      ~unordered_set() noexcept;

      // Assign

      unordered_set& operator=(unordered_set const& x)
      {
        table_.assign(x.table_, std::true_type());
        return *this;
      }

      unordered_set& operator=(unordered_set&& x)
        noexcept(value_allocator_traits::is_always_equal::value&&
            std::is_nothrow_move_assignable<H>::value&&
              std::is_nothrow_move_assignable<P>::value)
      {
        table_.move_assign(x.table_, std::true_type());
        return *this;
      }

      unordered_set& operator=(std::initializer_list<value_type>);

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

      template <class Key>
      typename boost::enable_if_c<
        detail::transparent_non_iterable<Key, unordered_set>::value,
        std::pair<iterator, bool> >::type
      insert(Key&& k)
      {
        return table_.try_emplace_unique(std::forward<Key>(k));
      }

      iterator insert(const_iterator hint, value_type const& x)
      {
        return this->emplace_hint(hint, x);
      }

      iterator insert(const_iterator hint, value_type&& x)
      {
        return this->emplace_hint(hint, std::move(x));
      }

      template <class Key>
      typename boost::enable_if_c<
        detail::transparent_non_iterable<Key, unordered_set>::value,
        iterator>::type
      insert(const_iterator hint, Key&& k)
      {
        return table_.try_emplace_hint_unique(hint, std::forward<Key>(k));
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
        detail::transparent_non_iterable<Key, unordered_set>::value,
        node_type>::type
      extract(const Key& k)
      {
        return node_type(
          table_.extract_by_key_impl(k),
          allocator_type(table_.node_alloc()));
      }

      insert_return_type insert(node_type&& np)
      {
        insert_return_type result;
        table_.move_insert_node_type_unique(np, result);
        return result;
      }

      iterator insert(const_iterator hint, node_type&& np)
      {
        return table_.move_insert_node_type_with_hint_unique(hint, np);
      }

      iterator erase(const_iterator);
      size_type erase(const key_type&);
      iterator erase(const_iterator, const_iterator);

      template <class Key>
      typename boost::enable_if_c<
        detail::transparent_non_iterable<Key, unordered_set>::value,
        size_type>::type
      erase(Key&& k)
      {
        return table_.erase_key_unique_impl(std::forward<Key>(k));
      }

      BOOST_UNORDERED_DEPRECATED("Use erase instead")
      void quick_erase(const_iterator it) { erase(it); }
      BOOST_UNORDERED_DEPRECATED("Use erase instead")
      void erase_return_void(const_iterator it) { erase(it); }

      void swap(unordered_set&)
        noexcept(value_allocator_traits::is_always_equal::value&&
            boost::unordered::detail::is_nothrow_swappable<H>::value&&
              boost::unordered::detail::is_nothrow_swappable<P>::value);
      void clear() noexcept { table_.clear_impl(); }

      template <typename H2, typename P2>
      void merge(boost::unordered_set<T, H2, P2, A>& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_set<T, H2, P2, A>&& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_multiset<T, H2, P2, A>& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_multiset<T, H2, P2, A>&& source);

      // observers

      hasher hash_function() const;
      key_equal key_eq() const;

      // lookup

      const_iterator find(const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        const_iterator>::type
      find(const Key& k) const
      {
        return const_iterator(table_.find(k));
      }

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
        return table_.find(k) != this->end() ? 1 : 0;
      }

      std::pair<const_iterator, const_iterator> equal_range(
        const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        std::pair<const_iterator, const_iterator> >::type
      equal_range(Key const& k) const
      {
        iterator n = table_.find(k);
        iterator m = n;
        if (m != this->end()) {
          ++m;
        }

        return std::make_pair(const_iterator(n), const_iterator(m));
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
        <T, H, P, A>(unordered_set const&, unordered_set const&);
      friend bool operator!=
        <T, H, P, A>(unordered_set const&, unordered_set const&);
#endif
    }; // class template unordered_set

    template <class Archive, class K, class H, class P, class A>
    void serialize(
      Archive& ar, unordered_set<K, H, P, A>& c, unsigned int version)
    {
      detail::serialize_fca_container(ar, c, version);
    }

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
    unordered_set(InputIterator, InputIterator,
      std::size_t = boost::unordered::detail::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> unordered_set<typename std::iterator_traits<InputIterator>::value_type,
        Hash, Pred, Allocator>;

    template <class T, class Hash = boost::hash<T>,
      class Pred = std::equal_to<T>, class Allocator = std::allocator<T>,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_pred_v<Pred> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_set(std::initializer_list<T>,
      std::size_t = boost::unordered::detail::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> unordered_set<T, Hash, Pred, Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_set(InputIterator, InputIterator, std::size_t, Allocator)
      -> unordered_set<typename std::iterator_traits<InputIterator>::value_type,
        boost::hash<typename std::iterator_traits<InputIterator>::value_type>,
        std::equal_to<typename std::iterator_traits<InputIterator>::value_type>,
        Allocator>;

    template <class InputIterator, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_set(InputIterator, InputIterator, std::size_t, Hash, Allocator)
      -> unordered_set<typename std::iterator_traits<InputIterator>::value_type,
        Hash,
        std::equal_to<typename std::iterator_traits<InputIterator>::value_type>,
        Allocator>;

    template <class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_set(std::initializer_list<T>, std::size_t, Allocator)
      -> unordered_set<T, boost::hash<T>, std::equal_to<T>, Allocator>;

    template <class T, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_set(std::initializer_list<T>, std::size_t, Hash, Allocator)
      -> unordered_set<T, Hash, std::equal_to<T>, Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_set(InputIterator, InputIterator, Allocator)
      -> unordered_set<typename std::iterator_traits<InputIterator>::value_type,
        boost::hash<typename std::iterator_traits<InputIterator>::value_type>,
        std::equal_to<typename std::iterator_traits<InputIterator>::value_type>,
        Allocator>;

    template <class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_set(std::initializer_list<T>, Allocator)
      -> unordered_set<T, boost::hash<T>, std::equal_to<T>, Allocator>;

#endif

    template <class T, class H, class P, class A> class unordered_multiset
    {
      template <typename, typename, typename, typename>
      friend class unordered_set;

    public:
      typedef T key_type;
      typedef T value_type;
      typedef H hasher;
      typedef P key_equal;
      typedef A allocator_type;

    private:
      typedef boost::unordered::detail::set<A, T, H, P> types;
      typedef typename types::value_allocator_traits value_allocator_traits;
      typedef typename types::table table;

    public:
      typedef typename value_allocator_traits::pointer pointer;
      typedef typename value_allocator_traits::const_pointer const_pointer;

      typedef value_type& reference;
      typedef value_type const& const_reference;

      typedef std::size_t size_type;
      typedef std::ptrdiff_t difference_type;

      typedef typename table::c_iterator iterator;
      typedef typename table::c_iterator const_iterator;
      typedef typename table::cl_iterator local_iterator;
      typedef typename table::cl_iterator const_local_iterator;
      typedef typename types::node_type node_type;

    private:
      table table_;

    public:
      // constructors

      unordered_multiset();

      explicit unordered_multiset(size_type, const hasher& = hasher(),
        const key_equal& = key_equal(),
        const allocator_type& = allocator_type());

      template <class InputIt>
      unordered_multiset(InputIt, InputIt,
        size_type = boost::unordered::detail::default_bucket_count,
        const hasher& = hasher(), const key_equal& = key_equal(),
        const allocator_type& = allocator_type());

      unordered_multiset(unordered_multiset const&);

      unordered_multiset(unordered_multiset&& other)
        noexcept(table::nothrow_move_constructible)
          : table_(other.table_, boost::unordered::detail::move_tag())
      {
        // The move is done in table_
      }

      explicit unordered_multiset(allocator_type const&);

      unordered_multiset(unordered_multiset const&, allocator_type const&);

      unordered_multiset(unordered_multiset&&, allocator_type const&);

      unordered_multiset(std::initializer_list<value_type>,
        size_type = boost::unordered::detail::default_bucket_count,
        const hasher& = hasher(), const key_equal& l = key_equal(),
        const allocator_type& = allocator_type());

      explicit unordered_multiset(size_type, const allocator_type&);

      explicit unordered_multiset(
        size_type, const hasher&, const allocator_type&);

      template <class InputIterator>
      unordered_multiset(InputIterator, InputIterator, const allocator_type&);

      template <class InputIt>
      unordered_multiset(InputIt, InputIt, size_type, const allocator_type&);

      template <class InputIt>
      unordered_multiset(
        InputIt, InputIt, size_type, const hasher&, const allocator_type&);

      unordered_multiset(
        std::initializer_list<value_type>, const allocator_type&);

      unordered_multiset(
        std::initializer_list<value_type>, size_type, const allocator_type&);

      unordered_multiset(std::initializer_list<value_type>, size_type,
        const hasher&, const allocator_type&);

      // Destructor

      ~unordered_multiset() noexcept;

      // Assign
      unordered_multiset& operator=(unordered_multiset const& x)
      {
        table_.assign(x.table_, std::false_type());
        return *this;
      }

      unordered_multiset& operator=(unordered_multiset&& x)
        noexcept(value_allocator_traits::is_always_equal::value&&
            std::is_nothrow_move_assignable<H>::value&&
              std::is_nothrow_move_assignable<P>::value)
      {
        table_.move_assign(x.table_, std::false_type());
        return *this;
      }

      unordered_multiset& operator=(std::initializer_list<value_type>);

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

      iterator insert(const_iterator hint, value_type const& x)
      {
        return this->emplace_hint(hint, x);
      }

      iterator insert(const_iterator hint, value_type&& x)
      {
        return this->emplace_hint(hint, std::move(x));
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
        detail::transparent_non_iterable<Key, unordered_multiset>::value,
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

      iterator erase(const_iterator);
      size_type erase(const key_type&);

      template <class Key>
      typename boost::enable_if_c<
        detail::transparent_non_iterable<Key, unordered_multiset>::value,
        size_type>::type
      erase(const Key& k)
      {
        return table_.erase_key_equiv_impl(k);
      }

      iterator erase(const_iterator, const_iterator);
      BOOST_UNORDERED_DEPRECATED("Use erase instead")
      void quick_erase(const_iterator it) { erase(it); }
      BOOST_UNORDERED_DEPRECATED("Use erase instead")
      void erase_return_void(const_iterator it) { erase(it); }

      void swap(unordered_multiset&)
        noexcept(value_allocator_traits::is_always_equal::value&&
            boost::unordered::detail::is_nothrow_swappable<H>::value&&
              boost::unordered::detail::is_nothrow_swappable<P>::value);
      void clear() noexcept { table_.clear_impl(); }

      template <typename H2, typename P2>
      void merge(boost::unordered_multiset<T, H2, P2, A>& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_multiset<T, H2, P2, A>&& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_set<T, H2, P2, A>& source);

      template <typename H2, typename P2>
      void merge(boost::unordered_set<T, H2, P2, A>&& source);

      // observers

      hasher hash_function() const;
      key_equal key_eq() const;

      // lookup

      const_iterator find(const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        const_iterator>::type
      find(const Key& k) const
      {
        return table_.find(k);
      }

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
        return table_.group_count(k);
      }

      std::pair<const_iterator, const_iterator> equal_range(
        const key_type&) const;

      template <class Key>
      typename boost::enable_if_c<detail::are_transparent<Key, H, P>::value,
        std::pair<const_iterator, const_iterator> >::type
      equal_range(const Key& k) const
      {
        iterator first = table_.find(k);
        iterator last = table_.next_group(k, first);
        return std::make_pair(const_iterator(first), const_iterator(last));
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
        <T, H, P, A>(unordered_multiset const&, unordered_multiset const&);
      friend bool operator!=
        <T, H, P, A>(unordered_multiset const&, unordered_multiset const&);
#endif
    }; // class template unordered_multiset

    template <class Archive, class K, class H, class P, class A>
    void serialize(
      Archive& ar, unordered_multiset<K, H, P, A>& c, unsigned int version)
    {
      detail::serialize_fca_container(ar, c, version);
    }

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
    unordered_multiset(InputIterator, InputIterator,
      std::size_t = boost::unordered::detail::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> unordered_multiset<
        typename std::iterator_traits<InputIterator>::value_type, Hash, Pred,
        Allocator>;

    template <class T, class Hash = boost::hash<T>,
      class Pred = std::equal_to<T>, class Allocator = std::allocator<T>,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_pred_v<Pred> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multiset(std::initializer_list<T>,
      std::size_t = boost::unordered::detail::default_bucket_count,
      Hash = Hash(), Pred = Pred(), Allocator = Allocator())
      -> unordered_multiset<T, Hash, Pred, Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multiset(InputIterator, InputIterator, std::size_t, Allocator)
      -> unordered_multiset<
        typename std::iterator_traits<InputIterator>::value_type,
        boost::hash<typename std::iterator_traits<InputIterator>::value_type>,
        std::equal_to<typename std::iterator_traits<InputIterator>::value_type>,
        Allocator>;

    template <class InputIterator, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multiset(
      InputIterator, InputIterator, std::size_t, Hash, Allocator)
      -> unordered_multiset<
        typename std::iterator_traits<InputIterator>::value_type, Hash,
        std::equal_to<typename std::iterator_traits<InputIterator>::value_type>,
        Allocator>;

    template <class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multiset(std::initializer_list<T>, std::size_t, Allocator)
      -> unordered_multiset<T, boost::hash<T>, std::equal_to<T>, Allocator>;

    template <class T, class Hash, class Allocator,
      class = std::enable_if_t<detail::is_hash_v<Hash> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multiset(std::initializer_list<T>, std::size_t, Hash, Allocator)
      -> unordered_multiset<T, Hash, std::equal_to<T>, Allocator>;

    template <class InputIterator, class Allocator,
      class = std::enable_if_t<detail::is_input_iterator_v<InputIterator> >,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multiset(InputIterator, InputIterator, Allocator)
      -> unordered_multiset<
        typename std::iterator_traits<InputIterator>::value_type,
        boost::hash<typename std::iterator_traits<InputIterator>::value_type>,
        std::equal_to<typename std::iterator_traits<InputIterator>::value_type>,
        Allocator>;

    template <class T, class Allocator,
      class = std::enable_if_t<detail::is_allocator_v<Allocator> > >
    unordered_multiset(std::initializer_list<T>, Allocator)
      -> unordered_multiset<T, boost::hash<T>, std::equal_to<T>, Allocator>;

#endif

    ////////////////////////////////////////////////////////////////////////////
    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set()
    {
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(size_type n, const hasher& hf,
      const key_equal& eql, const allocator_type& a)
        : table_(n, hf, eql, a)
    {
    }

    template <class T, class H, class P, class A>
    template <class InputIt>
    unordered_set<T, H, P, A>::unordered_set(InputIt f, InputIt l, size_type n,
      const hasher& hf, const key_equal& eql, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(f, l, n), hf, eql, a)
    {
      this->insert(f, l);
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(unordered_set const& other)
        : table_(other.table_,
            unordered_set::value_allocator_traits::
              select_on_container_copy_construction(other.get_allocator()))
    {
      if (other.size()) {
        table_.copy_buckets(other.table_, std::true_type());
      }
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(allocator_type const& a)
        : table_(boost::unordered::detail::default_bucket_count, hasher(),
            key_equal(), a)
    {
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(
      unordered_set const& other, allocator_type const& a)
        : table_(other.table_, a)
    {
      if (other.table_.size_) {
        table_.copy_buckets(other.table_, std::true_type());
      }
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(
      unordered_set&& other, allocator_type const& a)
        : table_(other.table_, a, boost::unordered::detail::move_tag())
    {
      table_.move_construct_buckets(other.table_);
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(
      std::initializer_list<value_type> list, size_type n, const hasher& hf,
      const key_equal& eql, const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hf, eql, a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(
      size_type n, const allocator_type& a)
        : table_(n, hasher(), key_equal(), a)
    {
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(
      size_type n, const hasher& hf, const allocator_type& a)
        : table_(n, hf, key_equal(), a)
    {
    }

    template <class T, class H, class P, class A>
    template <class InputIterator>
    unordered_set<T, H, P, A>::unordered_set(
      InputIterator f, InputIterator l, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(
                   f, l, detail::default_bucket_count),
            hasher(), key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class T, class H, class P, class A>
    template <class InputIt>
    unordered_set<T, H, P, A>::unordered_set(
      InputIt f, InputIt l, size_type n, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(f, l, n), hasher(),
            key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class T, class H, class P, class A>
    template <class InputIt>
    unordered_set<T, H, P, A>::unordered_set(InputIt f, InputIt l, size_type n,
      const hasher& hf, const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(f, l, n), hf, key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(
      std::initializer_list<value_type> list, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(
                   list.begin(), list.end(), detail::default_bucket_count),
            hasher(), key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(
      std::initializer_list<value_type> list, size_type n,
      const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hasher(), key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::unordered_set(
      std::initializer_list<value_type> list, size_type n, const hasher& hf,
      const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hf, key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>::~unordered_set() noexcept
    {
    }

    template <class T, class H, class P, class A>
    unordered_set<T, H, P, A>& unordered_set<T, H, P, A>::operator=(
      std::initializer_list<value_type> list)
    {
      this->clear();
      this->insert(list.begin(), list.end());
      return *this;
    }

    // size and capacity

    template <class T, class H, class P, class A>
    std::size_t unordered_set<T, H, P, A>::max_size() const noexcept
    {
      using namespace std;

      // size < mlf_ * count
      return boost::unordered::detail::double_to_size(
               ceil(static_cast<double>(table_.mlf_) *
                    static_cast<double>(table_.max_bucket_count()))) -
             1;
    }

    // modifiers

    template <class T, class H, class P, class A>
    template <class InputIt>
    void unordered_set<T, H, P, A>::insert(InputIt first, InputIt last)
    {
      if (first != last) {
        table_.insert_range_unique(
          table::extractor::extract(*first), first, last);
      }
    }

    template <class T, class H, class P, class A>
    void unordered_set<T, H, P, A>::insert(
      std::initializer_list<value_type> list)
    {
      this->insert(list.begin(), list.end());
    }

    template <class T, class H, class P, class A>
    typename unordered_set<T, H, P, A>::iterator
    unordered_set<T, H, P, A>::erase(const_iterator position)
    {
      return table_.erase_node(position);
    }

    template <class T, class H, class P, class A>
    typename unordered_set<T, H, P, A>::size_type
    unordered_set<T, H, P, A>::erase(const key_type& k)
    {
      return table_.erase_key_unique_impl(k);
    }

    template <class T, class H, class P, class A>
    typename unordered_set<T, H, P, A>::iterator
    unordered_set<T, H, P, A>::erase(const_iterator first, const_iterator last)
    {
      return table_.erase_nodes_range(first, last);
    }

    template <class T, class H, class P, class A>
    void unordered_set<T, H, P, A>::swap(unordered_set& other)
      noexcept(value_allocator_traits::is_always_equal::value&&
          boost::unordered::detail::is_nothrow_swappable<H>::value&&
            boost::unordered::detail::is_nothrow_swappable<P>::value)
    {
      table_.swap(other.table_);
    }

    // observers

    template <class T, class H, class P, class A>
    typename unordered_set<T, H, P, A>::hasher
    unordered_set<T, H, P, A>::hash_function() const
    {
      return table_.hash_function();
    }

    template <class T, class H, class P, class A>
    typename unordered_set<T, H, P, A>::key_equal
    unordered_set<T, H, P, A>::key_eq() const
    {
      return table_.key_eq();
    }

    template <class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_set<T, H, P, A>::merge(
      boost::unordered_set<T, H2, P2, A>& source)
    {
      table_.merge_unique(source.table_);
    }

    template <class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_set<T, H, P, A>::merge(
      boost::unordered_set<T, H2, P2, A>&& source)
    {
      table_.merge_unique(source.table_);
    }

    template <class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_set<T, H, P, A>::merge(
      boost::unordered_multiset<T, H2, P2, A>& source)
    {
      table_.merge_unique(source.table_);
    }

    template <class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_set<T, H, P, A>::merge(
      boost::unordered_multiset<T, H2, P2, A>&& source)
    {
      table_.merge_unique(source.table_);
    }

    // lookup

    template <class T, class H, class P, class A>
    typename unordered_set<T, H, P, A>::const_iterator
    unordered_set<T, H, P, A>::find(const key_type& k) const
    {
      return const_iterator(table_.find(k));
    }

    template <class T, class H, class P, class A>
    template <class CompatibleKey, class CompatibleHash,
      class CompatiblePredicate>
    typename unordered_set<T, H, P, A>::const_iterator
    unordered_set<T, H, P, A>::find(CompatibleKey const& k,
      CompatibleHash const& hash, CompatiblePredicate const& eq) const
    {
      return table_.transparent_find(k, hash, eq);
    }

    template <class T, class H, class P, class A>
    typename unordered_set<T, H, P, A>::size_type
    unordered_set<T, H, P, A>::count(const key_type& k) const
    {
      return table_.find_node(k) ? 1 : 0;
    }

    template <class T, class H, class P, class A>
    std::pair<typename unordered_set<T, H, P, A>::const_iterator,
      typename unordered_set<T, H, P, A>::const_iterator>
    unordered_set<T, H, P, A>::equal_range(const key_type& k) const
    {
      iterator first = table_.find(k);
      iterator second = first;
      if (second != this->end()) {
        ++second;
      }
      return std::make_pair(first, second);
    }

    template <class T, class H, class P, class A>
    typename unordered_set<T, H, P, A>::size_type
    unordered_set<T, H, P, A>::bucket_size(size_type n) const
    {
      return table_.bucket_size(n);
    }

    // hash policy

    template <class T, class H, class P, class A>
    float unordered_set<T, H, P, A>::load_factor() const noexcept
    {
      if (table_.size_ == 0) {
        return 0.0f;
      }

      BOOST_ASSERT(table_.bucket_count() != 0);
      return static_cast<float>(table_.size_) /
             static_cast<float>(table_.bucket_count());
    }

    template <class T, class H, class P, class A>
    void unordered_set<T, H, P, A>::max_load_factor(float m) noexcept
    {
      table_.max_load_factor(m);
    }

    template <class T, class H, class P, class A>
    void unordered_set<T, H, P, A>::rehash(size_type n)
    {
      table_.rehash(n);
    }

    template <class T, class H, class P, class A>
    void unordered_set<T, H, P, A>::reserve(size_type n)
    {
      table_.reserve(n);
    }

    template <class T, class H, class P, class A>
    inline bool operator==(
      unordered_set<T, H, P, A> const& m1, unordered_set<T, H, P, A> const& m2)
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_set<T, H, P, A> x;
      };
#endif
      return m1.table_.equals_unique(m2.table_);
    }

    template <class T, class H, class P, class A>
    inline bool operator!=(
      unordered_set<T, H, P, A> const& m1, unordered_set<T, H, P, A> const& m2)
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_set<T, H, P, A> x;
      };
#endif
      return !m1.table_.equals_unique(m2.table_);
    }

    template <class T, class H, class P, class A>
    inline void swap(unordered_set<T, H, P, A>& m1,
      unordered_set<T, H, P, A>& m2) noexcept(noexcept(m1.swap(m2)))
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_set<T, H, P, A> x;
      };
#endif
      m1.swap(m2);
    }

    template <class K, class H, class P, class A, class Predicate>
    typename unordered_set<K, H, P, A>::size_type erase_if(
      unordered_set<K, H, P, A>& c, Predicate pred)
    {
      return detail::erase_if(c, pred);
    }

    ////////////////////////////////////////////////////////////////////////////

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset()
    {
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(size_type n,
      const hasher& hf, const key_equal& eql, const allocator_type& a)
        : table_(n, hf, eql, a)
    {
    }

    template <class T, class H, class P, class A>
    template <class InputIt>
    unordered_multiset<T, H, P, A>::unordered_multiset(InputIt f, InputIt l,
      size_type n, const hasher& hf, const key_equal& eql,
      const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(f, l, n), hf, eql, a)
    {
      this->insert(f, l);
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      unordered_multiset const& other)
        : table_(other.table_,
            unordered_multiset::value_allocator_traits::
              select_on_container_copy_construction(other.get_allocator()))
    {
      if (other.table_.size_) {
        table_.copy_buckets(other.table_, std::false_type());
      }
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(allocator_type const& a)
        : table_(boost::unordered::detail::default_bucket_count, hasher(),
            key_equal(), a)
    {
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      unordered_multiset const& other, allocator_type const& a)
        : table_(other.table_, a)
    {
      if (other.table_.size_) {
        table_.copy_buckets(other.table_, std::false_type());
      }
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      unordered_multiset&& other, allocator_type const& a)
        : table_(other.table_, a, boost::unordered::detail::move_tag())
    {
      table_.move_construct_buckets(other.table_);
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      std::initializer_list<value_type> list, size_type n, const hasher& hf,
      const key_equal& eql, const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hf, eql, a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      size_type n, const allocator_type& a)
        : table_(n, hasher(), key_equal(), a)
    {
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      size_type n, const hasher& hf, const allocator_type& a)
        : table_(n, hf, key_equal(), a)
    {
    }

    template <class T, class H, class P, class A>
    template <class InputIterator>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      InputIterator f, InputIterator l, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(
                   f, l, detail::default_bucket_count),
            hasher(), key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class T, class H, class P, class A>
    template <class InputIt>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      InputIt f, InputIt l, size_type n, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(f, l, n), hasher(),
            key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class T, class H, class P, class A>
    template <class InputIt>
    unordered_multiset<T, H, P, A>::unordered_multiset(InputIt f, InputIt l,
      size_type n, const hasher& hf, const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(f, l, n), hf, key_equal(), a)
    {
      this->insert(f, l);
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      std::initializer_list<value_type> list, const allocator_type& a)
        : table_(boost::unordered::detail::initial_size(
                   list.begin(), list.end(), detail::default_bucket_count),
            hasher(), key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      std::initializer_list<value_type> list, size_type n,
      const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hasher(), key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::unordered_multiset(
      std::initializer_list<value_type> list, size_type n, const hasher& hf,
      const allocator_type& a)
        : table_(
            boost::unordered::detail::initial_size(list.begin(), list.end(), n),
            hf, key_equal(), a)
    {
      this->insert(list.begin(), list.end());
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>::~unordered_multiset() noexcept
    {
    }

    template <class T, class H, class P, class A>
    unordered_multiset<T, H, P, A>& unordered_multiset<T, H, P, A>::operator=(
      std::initializer_list<value_type> list)
    {
      this->clear();
      this->insert(list.begin(), list.end());
      return *this;
    }

    // size and capacity

    template <class T, class H, class P, class A>
    std::size_t unordered_multiset<T, H, P, A>::max_size() const noexcept
    {
      using namespace std;

      // size < mlf_ * count
      return boost::unordered::detail::double_to_size(
               ceil(static_cast<double>(table_.mlf_) *
                    static_cast<double>(table_.max_bucket_count()))) -
             1;
    }

    // modifiers

    template <class T, class H, class P, class A>
    template <class InputIt>
    void unordered_multiset<T, H, P, A>::insert(InputIt first, InputIt last)
    {
      table_.insert_range_equiv(first, last);
    }

    template <class T, class H, class P, class A>
    void unordered_multiset<T, H, P, A>::insert(
      std::initializer_list<value_type> list)
    {
      this->insert(list.begin(), list.end());
    }

    template <class T, class H, class P, class A>
    typename unordered_multiset<T, H, P, A>::iterator
    unordered_multiset<T, H, P, A>::erase(const_iterator position)
    {
      BOOST_ASSERT(position != this->end());
      return table_.erase_node(position);
    }

    template <class T, class H, class P, class A>
    typename unordered_multiset<T, H, P, A>::size_type
    unordered_multiset<T, H, P, A>::erase(const key_type& k)
    {
      return table_.erase_key_equiv(k);
    }

    template <class T, class H, class P, class A>
    typename unordered_multiset<T, H, P, A>::iterator
    unordered_multiset<T, H, P, A>::erase(
      const_iterator first, const_iterator last)
    {
      return table_.erase_nodes_range(first, last);
    }

    template <class T, class H, class P, class A>
    void unordered_multiset<T, H, P, A>::swap(unordered_multiset& other)
      noexcept(value_allocator_traits::is_always_equal::value&&
          boost::unordered::detail::is_nothrow_swappable<H>::value&&
            boost::unordered::detail::is_nothrow_swappable<P>::value)
    {
      table_.swap(other.table_);
    }

    // observers

    template <class T, class H, class P, class A>
    typename unordered_multiset<T, H, P, A>::hasher
    unordered_multiset<T, H, P, A>::hash_function() const
    {
      return table_.hash_function();
    }

    template <class T, class H, class P, class A>
    typename unordered_multiset<T, H, P, A>::key_equal
    unordered_multiset<T, H, P, A>::key_eq() const
    {
      return table_.key_eq();
    }

    template <class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_multiset<T, H, P, A>::merge(
      boost::unordered_multiset<T, H2, P2, A>& source)
    {
      while (!source.empty()) {
        insert(source.extract(source.begin()));
      }
    }

    template <class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_multiset<T, H, P, A>::merge(
      boost::unordered_multiset<T, H2, P2, A>&& source)
    {
      while (!source.empty()) {
        insert(source.extract(source.begin()));
      }
    }

    template <class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_multiset<T, H, P, A>::merge(
      boost::unordered_set<T, H2, P2, A>& source)
    {
      while (!source.empty()) {
        insert(source.extract(source.begin()));
      }
    }

    template <class T, class H, class P, class A>
    template <typename H2, typename P2>
    void unordered_multiset<T, H, P, A>::merge(
      boost::unordered_set<T, H2, P2, A>&& source)
    {
      while (!source.empty()) {
        insert(source.extract(source.begin()));
      }
    }

    // lookup

    template <class T, class H, class P, class A>
    typename unordered_multiset<T, H, P, A>::const_iterator
    unordered_multiset<T, H, P, A>::find(const key_type& k) const
    {
      return const_iterator(table_.find(k));
    }

    template <class T, class H, class P, class A>
    template <class CompatibleKey, class CompatibleHash,
      class CompatiblePredicate>
    typename unordered_multiset<T, H, P, A>::const_iterator
    unordered_multiset<T, H, P, A>::find(CompatibleKey const& k,
      CompatibleHash const& hash, CompatiblePredicate const& eq) const
    {
      return table_.transparent_find(k, hash, eq);
    }

    template <class T, class H, class P, class A>
    typename unordered_multiset<T, H, P, A>::size_type
    unordered_multiset<T, H, P, A>::count(const key_type& k) const
    {
      return table_.group_count(k);
    }

    template <class T, class H, class P, class A>
    std::pair<typename unordered_multiset<T, H, P, A>::const_iterator,
      typename unordered_multiset<T, H, P, A>::const_iterator>
    unordered_multiset<T, H, P, A>::equal_range(const key_type& k) const
    {
      iterator n = table_.find(k);
      return std::make_pair(const_iterator(n),
        const_iterator(n == end() ? n : table_.next_group(k, n)));
    }

    template <class T, class H, class P, class A>
    typename unordered_multiset<T, H, P, A>::size_type
    unordered_multiset<T, H, P, A>::bucket_size(size_type n) const
    {
      return table_.bucket_size(n);
    }

    // hash policy

    template <class T, class H, class P, class A>
    float unordered_multiset<T, H, P, A>::load_factor() const noexcept
    {
      if (table_.size_ == 0) {
        return 0.0f;
      }

      BOOST_ASSERT(table_.bucket_count() != 0);
      return static_cast<float>(table_.size_) /
             static_cast<float>(table_.bucket_count());
    }

    template <class T, class H, class P, class A>
    void unordered_multiset<T, H, P, A>::max_load_factor(float m) noexcept
    {
      table_.max_load_factor(m);
    }

    template <class T, class H, class P, class A>
    void unordered_multiset<T, H, P, A>::rehash(size_type n)
    {
      table_.rehash(n);
    }

    template <class T, class H, class P, class A>
    void unordered_multiset<T, H, P, A>::reserve(size_type n)
    {
      table_.reserve(n);
    }

    template <class T, class H, class P, class A>
    inline bool operator==(unordered_multiset<T, H, P, A> const& m1,
      unordered_multiset<T, H, P, A> const& m2)
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_multiset<T, H, P, A> x;
      };
#endif
      return m1.table_.equals_equiv(m2.table_);
    }

    template <class T, class H, class P, class A>
    inline bool operator!=(unordered_multiset<T, H, P, A> const& m1,
      unordered_multiset<T, H, P, A> const& m2)
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_multiset<T, H, P, A> x;
      };
#endif
      return !m1.table_.equals_equiv(m2.table_);
    }

    template <class T, class H, class P, class A>
    inline void swap(unordered_multiset<T, H, P, A>& m1,
      unordered_multiset<T, H, P, A>& m2) noexcept(noexcept(m1.swap(m2)))
    {
#if BOOST_WORKAROUND(BOOST_CODEGEARC, BOOST_TESTED_AT(0x0613))
      struct dummy
      {
        unordered_multiset<T, H, P, A> x;
      };
#endif
      m1.swap(m2);
    }

    template <class K, class H, class P, class A, class Predicate>
    typename unordered_multiset<K, H, P, A>::size_type erase_if(
      unordered_multiset<K, H, P, A>& c, Predicate pred)
    {
      return detail::erase_if(c, pred);
    }

    template <typename N, typename T, typename A> class node_handle_set
    {
      template <typename Types> friend struct ::boost::unordered::detail::table;
      template <class T2, class H2, class P2, class A2>
      friend class unordered_set;
      template <class T2, class H2, class P2, class A2>
      friend class unordered_multiset;

      typedef typename boost::unordered::detail::rebind_wrap<A, T>::type
        value_allocator;
      typedef boost::unordered::detail::allocator_traits<value_allocator>
        value_allocator_traits;
      typedef N node;
      typedef typename boost::unordered::detail::rebind_wrap<A, node>::type
        node_allocator;
      typedef boost::unordered::detail::allocator_traits<node_allocator>
        node_allocator_traits;
      typedef typename node_allocator_traits::pointer node_pointer;

    public:
      typedef T value_type;
      typedef A allocator_type;

    private:
      node_pointer ptr_;
      bool has_alloc_;
      boost::unordered::detail::optional<value_allocator> alloc_;

      node_handle_set(node_pointer ptr, allocator_type const& a)
          : ptr_(ptr), alloc_(a)
      {
      }

    public:
      constexpr node_handle_set() noexcept : ptr_(), has_alloc_(false) {}
      node_handle_set(node_handle_set const&) = delete;
      node_handle_set& operator=(node_handle_set const&) = delete;

      ~node_handle_set()
      {
        if (ptr_) {
          node_allocator node_alloc(*alloc_);
          boost::unordered::detail::node_tmp<node_allocator> tmp(
            ptr_, node_alloc);
        }
      }

      node_handle_set(node_handle_set&& n) noexcept
          : ptr_(n.ptr_),
            alloc_(std::move(n.alloc_))
      {
        n.ptr_ = node_pointer();
      }

      node_handle_set& operator=(node_handle_set&& n)
      {
        BOOST_ASSERT(!alloc_.has_value() ||
                     value_allocator_traits::
                       propagate_on_container_move_assignment::value ||
                     (n.alloc_.has_value() && alloc_ == n.alloc_));

        if (ptr_) {
          node_allocator node_alloc(*alloc_);
          boost::unordered::detail::node_tmp<node_allocator> tmp(
            ptr_, node_alloc);
          ptr_ = node_pointer();
        }

        if (!alloc_.has_value() ||
            value_allocator_traits::propagate_on_container_move_assignment::
              value) {
          alloc_ = std::move(n.alloc_);
        }
        ptr_ = n.ptr_;
        n.ptr_ = node_pointer();

        return *this;
      }

      value_type& value() const { return ptr_->value(); }

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

      void swap(node_handle_set& n)
        noexcept(value_allocator_traits::propagate_on_container_swap::value ||
                 value_allocator_traits::is_always_equal::value)
      {
        BOOST_ASSERT(
          !alloc_.has_value() || !n.alloc_.has_value() ||
          value_allocator_traits::propagate_on_container_swap::value ||
          alloc_ == n.alloc_);
        if (value_allocator_traits::propagate_on_container_swap::value ||
            !alloc_.has_value() || !n.alloc_.has_value()) {
          boost::core::invoke_swap(alloc_, n.alloc_);
        }
        boost::core::invoke_swap(ptr_, n.ptr_);
      }
    };

    template <typename N, typename T, typename A>
    void swap(node_handle_set<N, T, A>& x, node_handle_set<N, T, A>& y)
      noexcept(noexcept(x.swap(y)))
    {
      x.swap(y);
    }

    template <class Iter, class NodeType> struct insert_return_type_set
    {
    public:
      Iter position;
      bool inserted;
      NodeType node;

      insert_return_type_set() : position(), inserted(false), node() {}
      insert_return_type_set(insert_return_type_set const&) = delete;
      insert_return_type_set& operator=(insert_return_type_set const&) = delete;

      insert_return_type_set(insert_return_type_set&& x) noexcept
          : position(x.position),
            inserted(x.inserted),
            node(std::move(x.node))
      {
      }

      insert_return_type_set& operator=(insert_return_type_set&& x)
      {
        inserted = x.inserted;
        position = x.position;
        node = std::move(x.node);
        return *this;
      }
    };

    template <class Iter, class NodeType>
    void swap(insert_return_type_set<Iter, NodeType>& x,
      insert_return_type_set<Iter, NodeType>& y)
    {
      boost::core::invoke_swap(x.node, y.node);
      boost::core::invoke_swap(x.inserted, y.inserted);
      boost::core::invoke_swap(x.position, y.position);
    }
  } // namespace unordered

  namespace serialization {
    template <class K, class H, class P, class A>
    struct version<boost::unordered_set<K, H, P, A> >
    {
      BOOST_STATIC_CONSTANT(int, value = 1);
    };

    template <class K, class H, class P, class A>
    struct version<boost::unordered_multiset<K, H, P, A> >
    {
      BOOST_STATIC_CONSTANT(int, value = 1);
    };
  } // namespace serialization

} // namespace boost

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

#endif // BOOST_UNORDERED_UNORDERED_SET_HPP_INCLUDED
