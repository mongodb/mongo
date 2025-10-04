// Copyright (C) 2022-2024 Joaquin M Lopez Munoz.
// Copyright (C) 2022 Christian Mazakas
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_FCA_HPP
#define BOOST_UNORDERED_DETAIL_FCA_HPP

/*

The general structure of the fast closed addressing implementation is that we
use straight-forward separate chaining (i.e. each bucket contains its own linked
list) and then improve iteration time by adding an array of "bucket groups".

A bucket group is a constant-width view into a subsection of the buckets array,
containing a bitmask that indicates which one of the buckets in the subsection
contains a list of nodes. This allows the code to test N buckets for occupancy
in a single operation. Additional speed can be found by inter-linking occupied
bucket groups with one another in a doubly-linked list. To this end, large
swathes of the bucket groups array no longer need to be iterated and have their
bitmasks examined for occupancy.

A bucket group iterator contains a pointer to a bucket group along with a
pointer into the buckets array. The iterator's bucket pointer is guaranteed to
point to a bucket within the bucket group's view of the array. To advance the
iterator, we need to determine if we need to skip to the next bucket group or
simply move to the next occupied bucket as denoted by the bitmask.

To accomplish this, we perform something roughly equivalent to this:
```
bucket_iterator itb = ...
bucket_pointer p = itb.p
bucket_group_pointer pbg = itb.pbg

offset = p - pbg->buckets
// because we wish to see if the _next_ bit in the mask is occupied, we'll
// generate a testing mask from the current offset + 1
//
testing_mask = reset_first_bits(offset + 1)
n = ctz(pbg->bitmask & testing_mask)

if (n < N) {
  p = pbg->buckets + n
} else {
  pbg = pbg->next
  p = pbg->buckets + ctz(pbg->bitmask)
}
```

`reset_first_bits` yields an unsigned integral with the first n bits set to 0
and then by counting the number of trailing zeroes when AND'd against the bucket
group's bitmask, we can derive the offset into the buckets array. When the
calculated offset is equal to N, we know we've reached the end of a bucket group
and we can advance to the next one.

This is a rough explanation for how iterator incrementation should work for a
fixed width size of N as 3 for the bucket groups
```
N = 3
p = buckets
pbg->bitmask = 0b101
pbg->buckets = buckets

offset = p - pbg->buckets // => 0
testing_mask = reset_first_bits(offset + 1) // reset_first_bits(1) => 0b110

x = bitmask & testing_mask // => 0b101 & 0b110 => 0b100
ctz(x) // ctz(0b100) => 2
// 2 < 3
=> p = pbg->buckets + 2

// increment again...
offset = p - pbg->buckets // => 2
testing_mask = reset_first_bits(offset + 1) // reset_first_bits(3) => 0b000

bitmask & testing_mask // 0b101 & 0b000 => 0b000
ctz(0b000) => 3
// 3 < 3 is false now
pbg = pbg->next
initial_offset = ctz(pbg->bitmask)
p = pbg->buckets + initial_offset
```

For `size_` number of buckets, there are `1 + (size_ / N)` bucket groups where
`N` is the width of a bucket group, determined at compile-time.

We allocate space for `size_ + 1` buckets, using the last one as a dummy bucket
which is kept permanently empty so it can act as a sentinel value in the
implementation of `iterator end();`. We set the last bucket group to act as a
sentinel.

```
num_groups = size_ / N + 1
groups = allocate(num_groups)
pbg = groups + (num_groups - 1)

// not guaranteed to point to exactly N buckets
pbg->buckets = buckets + N * (size_ / N)

// this marks the true end of the bucket array
buckets pbg->bitmask = set_bit(size_ % N)

// links in on itself
pbg->next = pbg->prev = pbg
```

To this end, we can devise a safe iteration scheme while also creating a useful
sentinel to use as the end iterator.

Otherwise, usage of the data structure is relatively straight-forward compared
to normal separate chaining implementations.

*/

#include <boost/unordered/detail/prime_fmod.hpp>
#include <boost/unordered/detail/serialize_tracked_address.hpp>
#include <boost/unordered/detail/opt_storage.hpp>

#include <boost/assert.hpp>
#include <boost/core/allocator_access.hpp>
#include <boost/core/bit.hpp>
#include <boost/core/empty_value.hpp>
#include <boost/core/invoke_swap.hpp>
#include <boost/core/no_exceptions_support.hpp>
#include <boost/core/serialization.hpp>
#include <boost/cstdint.hpp>

#include <boost/config.hpp>

#include <iterator>

namespace boost {
  namespace unordered {
    namespace detail {

      template <class ValueType, class VoidPtr> struct node
      {
        typedef ValueType value_type;
        typedef typename boost::pointer_traits<VoidPtr>::template rebind_to<
          node>::type node_pointer;

        node_pointer next;
        opt_storage<value_type> buf;

        node() noexcept : next(), buf() {}

        value_type* value_ptr() noexcept
        {
          return buf.address();
        }

        value_type& value() noexcept
        {
          return *buf.address();
        }
      };

      template <class Node, class VoidPtr> struct bucket
      {
        typedef typename boost::pointer_traits<VoidPtr>::template rebind_to<
          Node>::type node_pointer;

        typedef typename boost::pointer_traits<VoidPtr>::template rebind_to<
          bucket>::type bucket_pointer;

        node_pointer next;

        bucket() noexcept : next() {}
      };

      template <class Bucket> struct bucket_group
      {
        typedef typename Bucket::bucket_pointer bucket_pointer;
        typedef
          typename boost::pointer_traits<bucket_pointer>::template rebind_to<
            bucket_group>::type bucket_group_pointer;

        BOOST_STATIC_CONSTANT(std::size_t, N = sizeof(std::size_t) * CHAR_BIT);

        bucket_pointer buckets;
        std::size_t bitmask;
        bucket_group_pointer next, prev;

        bucket_group() noexcept : buckets(), bitmask(0), next(), prev() {}
        ~bucket_group() {}
      };

      inline std::size_t set_bit(std::size_t n) { return std::size_t(1) << n; }

      inline std::size_t reset_bit(std::size_t n)
      {
        return ~(std::size_t(1) << n);
      }

      inline std::size_t reset_first_bits(std::size_t n) // n>0
      {
        return ~(~(std::size_t(0)) >> (sizeof(std::size_t) * 8 - n));
      }

      template <class Bucket> struct grouped_bucket_iterator
      {
      public:
        typedef typename Bucket::bucket_pointer bucket_pointer;
        typedef
          typename boost::pointer_traits<bucket_pointer>::template rebind_to<
            bucket_group<Bucket> >::type bucket_group_pointer;

        typedef Bucket value_type;
        typedef typename boost::pointer_traits<bucket_pointer>::difference_type
          difference_type;
        typedef Bucket& reference;
        typedef Bucket* pointer;
        typedef std::forward_iterator_tag iterator_category;

      private:
        bucket_pointer p;
        bucket_group_pointer pbg;

      public:
        grouped_bucket_iterator() : p(), pbg() {}

        reference operator*() const noexcept { return dereference(); }
        pointer operator->() const noexcept { return boost::to_address(p); }

        grouped_bucket_iterator& operator++() noexcept
        {
          increment();
          return *this;
        }

        grouped_bucket_iterator operator++(int) noexcept
        {
          grouped_bucket_iterator old = *this;
          increment();
          return old;
        }

        bool operator==(grouped_bucket_iterator const& other) const noexcept
        {
          return equal(other);
        }

        bool operator!=(grouped_bucket_iterator const& other) const noexcept
        {
          return !equal(other);
        }

      private:
        template <typename, typename, typename>
        friend class grouped_bucket_array;

        BOOST_STATIC_CONSTANT(std::size_t, N = bucket_group<Bucket>::N);

        grouped_bucket_iterator(bucket_pointer p_, bucket_group_pointer pbg_)
            : p(p_), pbg(pbg_)
        {
        }

        Bucket& dereference() const noexcept { return *p; }

        bool equal(const grouped_bucket_iterator& x) const noexcept
        {
          return p == x.p;
        }

        void increment() noexcept
        {
          std::size_t const offset = static_cast<std::size_t>(p - pbg->buckets);

          std::size_t n = std::size_t(boost::core::countr_zero(
            pbg->bitmask & reset_first_bits(offset + 1)));

          if (n < N) {
            p = pbg->buckets + static_cast<difference_type>(n);
          } else {
            pbg = pbg->next;

            std::ptrdiff_t x = boost::core::countr_zero(pbg->bitmask);
            p = pbg->buckets + x;
          }
        }

        template <typename Archive>
        friend void serialization_track(
          Archive& ar, grouped_bucket_iterator const& x)
        {
          // requires: not at end() position
          track_address(ar, x.p);
          track_address(ar, x.pbg);
        }

        friend class boost::serialization::access;

        template <typename Archive> void serialize(Archive& ar, unsigned int)
        {
          // requires: not at end() position
          serialize_tracked_address(ar, p);
          serialize_tracked_address(ar, pbg);
        }
      };

      template <class Node> struct const_grouped_local_bucket_iterator;

      template <class Node> struct grouped_local_bucket_iterator
      {
        typedef typename Node::node_pointer node_pointer;

      public:
        typedef typename Node::value_type value_type;
        typedef value_type element_type;
        typedef value_type* pointer;
        typedef value_type& reference;
        typedef std::ptrdiff_t difference_type;
        typedef std::forward_iterator_tag iterator_category;

        grouped_local_bucket_iterator() : p() {}

        reference operator*() const noexcept { return dereference(); }

        pointer operator->() const noexcept
        {
          return std::addressof(dereference());
        }

        grouped_local_bucket_iterator& operator++() noexcept
        {
          increment();
          return *this;
        }

        grouped_local_bucket_iterator operator++(int) noexcept
        {
          grouped_local_bucket_iterator old = *this;
          increment();
          return old;
        }

        bool operator==(
          grouped_local_bucket_iterator const& other) const noexcept
        {
          return equal(other);
        }

        bool operator!=(
          grouped_local_bucket_iterator const& other) const noexcept
        {
          return !equal(other);
        }

      private:
        template <typename, typename, typename>
        friend class grouped_bucket_array;

        template <class> friend struct const_grouped_local_bucket_iterator;

        grouped_local_bucket_iterator(node_pointer p_) : p(p_) {}

        value_type& dereference() const noexcept { return p->value(); }

        bool equal(const grouped_local_bucket_iterator& x) const noexcept
        {
          return p == x.p;
        }

        void increment() noexcept { p = p->next; }

        node_pointer p;
      };

      template <class Node> struct const_grouped_local_bucket_iterator
      {
        typedef typename Node::node_pointer node_pointer;

      public:
        typedef typename Node::value_type const value_type;
        typedef value_type const element_type;
        typedef value_type const* pointer;
        typedef value_type const& reference;
        typedef std::ptrdiff_t difference_type;
        typedef std::forward_iterator_tag iterator_category;

        const_grouped_local_bucket_iterator() : p() {}
        const_grouped_local_bucket_iterator(
          grouped_local_bucket_iterator<Node> it)
            : p(it.p)
        {
        }

        reference operator*() const noexcept { return dereference(); }

        pointer operator->() const noexcept
        {
          return std::addressof(dereference());
        }

        const_grouped_local_bucket_iterator& operator++() noexcept
        {
          increment();
          return *this;
        }

        const_grouped_local_bucket_iterator operator++(int) noexcept
        {
          const_grouped_local_bucket_iterator old = *this;
          increment();
          return old;
        }

        bool operator==(
          const_grouped_local_bucket_iterator const& other) const noexcept
        {
          return equal(other);
        }

        bool operator!=(
          const_grouped_local_bucket_iterator const& other) const noexcept
        {
          return !equal(other);
        }

      private:
        template <typename, typename, typename>
        friend class grouped_bucket_array;

        const_grouped_local_bucket_iterator(node_pointer p_) : p(p_) {}

        value_type& dereference() const noexcept { return p->value(); }

        bool equal(const const_grouped_local_bucket_iterator& x) const noexcept
        {
          return p == x.p;
        }

        void increment() noexcept { p = p->next; }

        node_pointer p;
      };

      template <class T> struct span
      {
        T* begin() const noexcept { return data; }
        T* end() const noexcept { return data + size; }

        T* data;
        std::size_t size;

        span(T* data_, std::size_t size_) : data(data_), size(size_) {}
      };

      template <class Bucket, class Allocator, class SizePolicy>
      class grouped_bucket_array
          : boost::empty_value<typename boost::allocator_rebind<Allocator,
              node<typename boost::allocator_value_type<Allocator>::type,
                typename boost::allocator_void_pointer<Allocator>::type> >::
                type>
      {
        typedef typename boost::allocator_value_type<Allocator>::type
          allocator_value_type;
        typedef
          typename boost::allocator_void_pointer<Allocator>::type void_pointer;
        typedef typename boost::allocator_difference_type<Allocator>::type
          difference_type;

      public:
        typedef typename boost::allocator_rebind<Allocator,
          node<allocator_value_type, void_pointer> >::type node_allocator_type;

        typedef node<allocator_value_type, void_pointer> node_type;
        typedef typename boost::allocator_pointer<node_allocator_type>::type
          node_pointer;
        typedef SizePolicy size_policy;

      private:
        typedef typename boost::allocator_rebind<Allocator, Bucket>::type
          bucket_allocator_type;
        typedef typename boost::allocator_pointer<bucket_allocator_type>::type
          bucket_pointer;
        typedef boost::pointer_traits<bucket_pointer> bucket_pointer_traits;

        typedef bucket_group<Bucket> group;
        typedef typename boost::allocator_rebind<Allocator, group>::type
          group_allocator_type;
        typedef typename boost::allocator_pointer<group_allocator_type>::type
          group_pointer;
        typedef typename boost::pointer_traits<group_pointer>
          group_pointer_traits;

      public:
        typedef Bucket value_type;
        typedef Bucket bucket_type;
        typedef std::size_t size_type;
        typedef Allocator allocator_type;
        typedef grouped_bucket_iterator<Bucket> iterator;
        typedef grouped_local_bucket_iterator<node_type> local_iterator;
        typedef const_grouped_local_bucket_iterator<node_type>
          const_local_iterator;

      private:
        std::size_t size_index_, size_;
        bucket_pointer buckets;
        group_pointer groups;

      public:
        static std::size_t bucket_count_for(std::size_t num_buckets)
        {
          if (num_buckets == 0) {
            return 0;
          }
          return size_policy::size(size_policy::size_index(num_buckets));
        }

        grouped_bucket_array()
            : empty_value<node_allocator_type>(
                empty_init_t(), node_allocator_type()),
              size_index_(0), size_(0), buckets(), groups()
        {
        }

        grouped_bucket_array(size_type n, const Allocator& al)
            : empty_value<node_allocator_type>(
                empty_init_t(), node_allocator_type(al)),
              size_index_(0), size_(0), buckets(), groups()
        {
          if (n == 0) {
            return;
          }

          size_index_ = size_policy::size_index(n);
          size_ = size_policy::size(size_index_);

          bucket_allocator_type bucket_alloc = this->get_bucket_allocator();
          group_allocator_type group_alloc = this->get_group_allocator();

          size_type const num_buckets = buckets_len();
          size_type const num_groups = groups_len();

          buckets = boost::allocator_allocate(bucket_alloc, num_buckets);
          BOOST_TRY
          {
            groups = boost::allocator_allocate(group_alloc, num_groups);

            bucket_type* pb = boost::to_address(buckets);
            for (size_type i = 0; i < num_buckets; ++i) {
              new (pb + i) bucket_type();
            }

            group* pg = boost::to_address(groups);
            for (size_type i = 0; i < num_groups; ++i) {
              new (pg + i) group();
            }
          }
          BOOST_CATCH(...)
          {
            boost::allocator_deallocate(bucket_alloc, buckets, num_buckets);
            BOOST_RETHROW
          }
          BOOST_CATCH_END

          size_type const N = group::N;
          group_pointer pbg =
            groups + static_cast<difference_type>(num_groups - 1);

          pbg->buckets =
            buckets + static_cast<difference_type>(N * (size_ / N));
          pbg->bitmask = set_bit(size_ % N);
          pbg->next = pbg->prev = pbg;
        }

        ~grouped_bucket_array() { this->deallocate(); }

        grouped_bucket_array(grouped_bucket_array const&) = delete;
        grouped_bucket_array& operator=(grouped_bucket_array const&) = delete;

        grouped_bucket_array(grouped_bucket_array&& other) noexcept
            : empty_value<node_allocator_type>(
                empty_init_t(), other.get_node_allocator()),
              size_index_(other.size_index_),
              size_(other.size_),
              buckets(other.buckets),
              groups(other.groups)
        {
          other.size_ = 0;
          other.size_index_ = 0;
          other.buckets = bucket_pointer();
          other.groups = group_pointer();
        }

        grouped_bucket_array& operator=(grouped_bucket_array&& other) noexcept
        {
          BOOST_ASSERT(
            this->get_node_allocator() == other.get_node_allocator());

          if (this == std::addressof(other)) {
            return *this;
          }

          this->deallocate();
          size_index_ = other.size_index_;
          size_ = other.size_;

          buckets = other.buckets;
          groups = other.groups;

          other.size_index_ = 0;
          other.size_ = 0;
          other.buckets = bucket_pointer();
          other.groups = group_pointer();

          return *this;
        }

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter (dtor calls)
#endif

        void deallocate() noexcept
        {
          if (buckets) {
            size_type const num_buckets = buckets_len();
            bucket_type* pb = boost::to_address(buckets);
            (void)pb; // VS complains when dtor is trivial

            for (size_type i = 0; i < num_buckets; ++i) {
              (pb + i)->~bucket_type();
            }

            bucket_allocator_type bucket_alloc = this->get_bucket_allocator();
            boost::allocator_deallocate(bucket_alloc, buckets, num_buckets);

            buckets = bucket_pointer();
          }

          if (groups) {
            size_type const num_groups = groups_len();
            group* pg = boost::to_address(groups);
            (void)pg; // VS complains when dtor is trivial

            for (size_type i = 0; i < num_groups; ++i) {
              (pg + i)->~group();
            }

            group_allocator_type group_alloc = this->get_group_allocator();
            boost::allocator_deallocate(group_alloc, groups, num_groups);

            groups = group_pointer();
          }
        }

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

        void swap(grouped_bucket_array& other)
        {
          std::swap(size_index_, other.size_index_);
          std::swap(size_, other.size_);
          std::swap(buckets, other.buckets);
          std::swap(groups, other.groups);

          swap_allocator_if_pocs(other);
        }

        node_allocator_type const& get_node_allocator() const
        {
          return empty_value<node_allocator_type>::get();
        }

        node_allocator_type& get_node_allocator()
        {
          return empty_value<node_allocator_type>::get();
        }

        bucket_allocator_type get_bucket_allocator() const
        {
          return bucket_allocator_type(this->get_node_allocator());
        }

        group_allocator_type get_group_allocator() const
        {
          return group_allocator_type(this->get_node_allocator());
        }

        Allocator get_allocator() const
        {
          return Allocator(this->get_node_allocator());
        }

        size_type buckets_len() const noexcept { return size_ + 1; }

        size_type groups_len() const noexcept { return size_ / group::N + 1; }

        void reset_allocator(Allocator const& allocator_)
        {
          this->get_node_allocator() = node_allocator_type(allocator_);
        }

        size_type bucket_count() const { return size_; }

        iterator begin() const { return size_ == 0 ? end() : ++at(size_); }

        iterator end() const
        {
          // micro optimization: no need to return the bucket group
          // as end() is not incrementable
          iterator pbg;
          pbg.p =
            buckets + static_cast<difference_type>(this->buckets_len() - 1);
          return pbg;
        }

        local_iterator begin(size_type n) const
        {
          if (size_ == 0) {
            return this->end(n);
          }

          return local_iterator(
            (buckets + static_cast<difference_type>(n))->next);
        }

        local_iterator end(size_type) const { return local_iterator(); }

        size_type capacity() const noexcept { return size_; }

        iterator at(size_type n) const
        {
          if (size_ > 0) {
            std::size_t const N = group::N;

            iterator pbg(buckets + static_cast<difference_type>(n),
              groups + static_cast<difference_type>(n / N));

            return pbg;
          } else {
            return this->end();
          }
        }

        span<Bucket> raw()
        {
          BOOST_ASSERT(size_ == 0 || size_ < this->buckets_len());
          return span<Bucket>(boost::to_address(buckets), size_);
        }

        size_type position(std::size_t hash) const
        {
          return size_policy::position(hash, size_index_);
        }

        void clear()
        {
          this->deallocate();
          size_index_ = 0;
          size_ = 0;
        }

        void append_bucket_group(iterator itb) noexcept
        {
          std::size_t const N = group::N;

          bool const is_empty_bucket = (!itb->next);
          if (is_empty_bucket) {
            bucket_pointer pb = itb.p;
            group_pointer pbg = itb.pbg;

            std::size_t n =
              static_cast<std::size_t>(boost::to_address(pb) - &buckets[0]);

            bool const is_empty_group = (!pbg->bitmask);
            if (is_empty_group) {
              size_type const num_groups = this->groups_len();
              group_pointer last_group =
                groups + static_cast<difference_type>(num_groups - 1);

              pbg->buckets =
                buckets + static_cast<difference_type>(N * (n / N));
              pbg->next = last_group->next;
              pbg->next->prev = pbg;
              pbg->prev = last_group;
              pbg->prev->next = pbg;
            }

            pbg->bitmask |= set_bit(n % N);
          }
        }

        void insert_node(iterator itb, node_pointer p) noexcept
        {
          this->append_bucket_group(itb);

          p->next = itb->next;
          itb->next = p;
        }

        void insert_node_hint(
          iterator itb, node_pointer p, node_pointer hint) noexcept
        {
          this->append_bucket_group(itb);

          if (hint) {
            p->next = hint->next;
            hint->next = p;
          } else {
            p->next = itb->next;
            itb->next = p;
          }
        }

        void extract_node(iterator itb, node_pointer p) noexcept
        {
          node_pointer* pp = std::addressof(itb->next);
          while ((*pp) != p)
            pp = std::addressof((*pp)->next);
          *pp = p->next;
          if (!itb->next)
            unlink_bucket(itb);
        }

        void extract_node_after(iterator itb, node_pointer* pp) noexcept
        {
          *pp = (*pp)->next;
          if (!itb->next)
            unlink_bucket(itb);
        }

        void unlink_empty_buckets() noexcept
        {
          std::size_t const N = group::N;

          group_pointer pbg = groups,
                        last = groups + static_cast<difference_type>(
                                          this->groups_len() - 1);

          for (; pbg != last; ++pbg) {
            if (!pbg->buckets) {
              continue;
            }

            for (std::size_t n = 0; n < N; ++n) {
              bucket_pointer bs = pbg->buckets;
              bucket_type& b = bs[static_cast<std::ptrdiff_t>(n)];
              if (!b.next)
                pbg->bitmask &= reset_bit(n);
            }
            if (!pbg->bitmask && pbg->next)
              unlink_group(pbg);
          }

          // do not check end bucket
          for (std::size_t n = 0; n < size_ % N; ++n) {
            if (!pbg->buckets[static_cast<std::ptrdiff_t>(n)].next)
              pbg->bitmask &= reset_bit(n);
          }
        }

        void unlink_bucket(iterator itb)
        {
          typename iterator::bucket_pointer p = itb.p;
          typename iterator::bucket_group_pointer pbg = itb.pbg;
          if (!(pbg->bitmask &=
                reset_bit(static_cast<std::size_t>(p - pbg->buckets))))
            unlink_group(pbg);
        }

      private:
        void unlink_group(group_pointer pbg)
        {
          pbg->next->prev = pbg->prev;
          pbg->prev->next = pbg->next;
          pbg->prev = pbg->next = group_pointer();
        }

        void swap_allocator_if_pocs(grouped_bucket_array& other)
        {
          using allocator_pocs =
            typename boost::allocator_propagate_on_container_swap<
              allocator_type>::type;
          swap_allocator_if_pocs(
            other, std::integral_constant<bool, allocator_pocs::value>());
        }

        void swap_allocator_if_pocs(
          grouped_bucket_array& other, std::true_type /* propagate */)
        {
          boost::core::invoke_swap(
            get_node_allocator(), other.get_node_allocator());
        }

        void swap_allocator_if_pocs(
          grouped_bucket_array&, std::false_type /* don't propagate */)
        {
        }
      };
    } // namespace detail
  } // namespace unordered
} // namespace boost

#endif // BOOST_UNORDERED_DETAIL_FCA_HPP
