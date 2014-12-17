
// Copyright (C) 2003-2004 Jeremy B. Maitin-Shepard.
// Copyright (C) 2005-2011 Daniel James
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_EQUIVALENT_HPP_INCLUDED
#define BOOST_UNORDERED_DETAIL_EQUIVALENT_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/unordered/detail/table.hpp>
#include <boost/unordered/detail/emplace_args.hpp>
#include <boost/unordered/detail/extract_key.hpp>

namespace boost { namespace unordered { namespace detail {

    template <typename A, typename T> struct grouped_node;
    template <typename T> struct grouped_ptr_node;
    template <typename Types> struct grouped_table_impl;

    template <typename A, typename T>
    struct grouped_node :
        boost::unordered::detail::value_base<T>
    {
        typedef typename ::boost::unordered::detail::rebind_wrap<
            A, grouped_node<A, T> >::type::pointer link_pointer;

        link_pointer next_;
        link_pointer group_prev_;
        std::size_t hash_;

        grouped_node() :
            next_(),
            group_prev_(),
            hash_(0)
        {}

        void init(link_pointer self)
        {
            group_prev_ = self;
        }
    };

    template <typename T>
    struct grouped_ptr_node :
        boost::unordered::detail::value_base<T>,
        boost::unordered::detail::ptr_bucket
    {
        typedef boost::unordered::detail::ptr_bucket bucket_base;
        typedef ptr_bucket* link_pointer;

        link_pointer group_prev_;
        std::size_t hash_;

        grouped_ptr_node() :
            bucket_base(),
            group_prev_(0),
            hash_(0)
        {}

        void init(link_pointer self)
        {
            group_prev_ = self;
        }
    };

    // If the allocator uses raw pointers use grouped_ptr_node
    // Otherwise use grouped_node.

    template <typename A, typename T, typename NodePtr, typename BucketPtr>
    struct pick_grouped_node2
    {
        typedef boost::unordered::detail::grouped_node<A, T> node;

        typedef typename boost::unordered::detail::allocator_traits<
            typename boost::unordered::detail::rebind_wrap<A, node>::type
        >::pointer node_pointer;

        typedef boost::unordered::detail::bucket<node_pointer> bucket;
        typedef node_pointer link_pointer;
    };

    template <typename A, typename T>
    struct pick_grouped_node2<A, T,
        boost::unordered::detail::grouped_ptr_node<T>*,
        boost::unordered::detail::ptr_bucket*>
    {
        typedef boost::unordered::detail::grouped_ptr_node<T> node;
        typedef boost::unordered::detail::ptr_bucket bucket;
        typedef bucket* link_pointer;
    };

    template <typename A, typename T>
    struct pick_grouped_node
    {
        typedef boost::unordered::detail::allocator_traits<
            typename boost::unordered::detail::rebind_wrap<A,
                boost::unordered::detail::grouped_ptr_node<T> >::type
        > tentative_node_traits;

        typedef boost::unordered::detail::allocator_traits<
            typename boost::unordered::detail::rebind_wrap<A,
                boost::unordered::detail::ptr_bucket >::type
        > tentative_bucket_traits;

        typedef pick_grouped_node2<A, T,
            typename tentative_node_traits::pointer,
            typename tentative_bucket_traits::pointer> pick;

        typedef typename pick::node node;
        typedef typename pick::bucket bucket;
        typedef typename pick::link_pointer link_pointer;
    };

    template <typename A, typename T, typename H, typename P>
    struct multiset
    {
        typedef boost::unordered::detail::multiset<A, T, H, P> types;

        typedef T value_type;
        typedef H hasher;
        typedef P key_equal;
        typedef T key_type;

        typedef typename boost::unordered::detail::rebind_wrap<
                A, value_type>::type allocator;

        typedef boost::unordered::detail::allocator_traits<allocator> traits;
        typedef boost::unordered::detail::pick_grouped_node<allocator, value_type> pick;
        typedef typename pick::node node;
        typedef typename pick::bucket bucket;
        typedef typename pick::link_pointer link_pointer;

        typedef boost::unordered::detail::grouped_table_impl<types> table;
        typedef boost::unordered::detail::set_extractor<value_type> extractor;
    };

    template <typename A, typename K, typename M, typename H, typename P>
    struct multimap
    {
        typedef boost::unordered::detail::multimap<A, K, M, H, P> types;

        typedef std::pair<K const, M> value_type;
        typedef H hasher;
        typedef P key_equal;
        typedef K key_type;

        typedef typename boost::unordered::detail::rebind_wrap<
                A, value_type>::type allocator;

        typedef boost::unordered::detail::allocator_traits<allocator> traits;
        typedef boost::unordered::detail::pick_grouped_node<allocator, value_type> pick;
        typedef typename pick::node node;
        typedef typename pick::bucket bucket;
        typedef typename pick::link_pointer link_pointer;

        typedef boost::unordered::detail::grouped_table_impl<types> table;
        typedef boost::unordered::detail::map_extractor<key_type, value_type>
            extractor;
    };

    template <typename Types>
    struct grouped_table_impl : boost::unordered::detail::table<Types>
    {
        typedef boost::unordered::detail::table<Types> table;
        typedef typename table::value_type value_type;
        typedef typename table::bucket bucket;
        typedef typename table::buckets buckets;
        typedef typename table::node_pointer node_pointer;
        typedef typename table::node_allocator node_allocator;
        typedef typename table::node_allocator_traits node_allocator_traits;
        typedef typename table::bucket_pointer bucket_pointer;
        typedef typename table::link_pointer link_pointer;
        typedef typename table::previous_pointer previous_pointer;
        typedef typename table::hasher hasher;
        typedef typename table::key_equal key_equal;
        typedef typename table::key_type key_type;
        typedef typename table::node_constructor node_constructor;
        typedef typename table::extractor extractor;
        typedef typename table::iterator iterator;

        // Constructors

        grouped_table_impl(std::size_t n,
                hasher const& hf,
                key_equal const& eq,
                node_allocator const& a)
          : table(n, hf, eq, a)
        {}

        grouped_table_impl(grouped_table_impl const& x)
          : table(x, node_allocator_traits::
                select_on_container_copy_construction(x.node_alloc())) {}

        grouped_table_impl(grouped_table_impl const& x,
                node_allocator const& a)
          : table(x, a)
        {}

        grouped_table_impl(grouped_table_impl& x,
                boost::unordered::detail::move_tag m)
          : table(x, m)
        {}

        grouped_table_impl(grouped_table_impl& x,
                node_allocator const& a,
                boost::unordered::detail::move_tag m)
          : table(x, a, m)
        {}

        // Accessors

        template <class Key, class Pred>
        node_pointer find_node_impl(
                std::size_t hash,
                Key const& k,
                Pred const& eq) const
        {
            std::size_t bucket_index = hash % this->bucket_count_;
            node_pointer n = this->get_start(bucket_index);

            for (;;)
            {
                if (!n) return n;

                std::size_t node_hash = n->hash_;
                if (hash == node_hash)
                {
                    if (eq(k, this->get_key(n->value())))
                        return n;
                }
                else
                {
                    if (node_hash % this->bucket_count_ != bucket_index)
                        return node_pointer();
                }

                n = static_cast<node_pointer>(
                    static_cast<node_pointer>(n->group_prev_)->next_);
            }
        }

        std::size_t count(key_type const& k) const
        {
            node_pointer n = this->find_node(k);
            if (!n) return 0;

            std::size_t count = 0;
            node_pointer it = n;
            do {
                it = static_cast<node_pointer>(it->group_prev_);
                ++count;
            } while(it != n);

            return count;
        }

        std::pair<iterator, iterator>
            equal_range(key_type const& k) const
        {
            node_pointer n = this->find_node(k);
            return std::make_pair(
                iterator(n), iterator(n ?
                    static_cast<node_pointer>(
                        static_cast<node_pointer>(n->group_prev_)->next_) :
                    n));
        }

        // Equality

        bool equals(grouped_table_impl const& other) const
        {
            if(this->size_ != other.size_) return false;
            if(!this->size_) return true;
    
            for(node_pointer n1 = this->get_start(); n1;)
            {
                node_pointer n2 = other.find_matching_node(n1);
                if (!n2) return false;
                node_pointer end1 = static_cast<node_pointer>(
                    static_cast<node_pointer>(n1->group_prev_)->next_);
                node_pointer end2 = static_cast<node_pointer>(
                    static_cast<node_pointer>(n2->group_prev_)->next_);
                if (!group_equals(n1, end1, n2, end2)) return false;
                n1 = end1;    
            }
    
            return true;
        }

#if !defined(BOOST_UNORDERED_DEPRECATED_EQUALITY)

        static bool group_equals(node_pointer n1, node_pointer end1,
                node_pointer n2, node_pointer end2)
        {
            for(;;)
            {
                if (n1->value() != n2->value())
                    break;

                n1 = static_cast<node_pointer>(n1->next_);
                n2 = static_cast<node_pointer>(n2->next_);
            
                if (n1 == end1) return n2 == end2;
                if (n2 == end2) return false;
            }
            
            for(node_pointer n1a = n1, n2a = n2;;)
            {
                n1a = static_cast<node_pointer>(n1a->next_);
                n2a = static_cast<node_pointer>(n2a->next_);

                if (n1a == end1)
                {
                    if (n2a == end2) break;
                    else return false;
                }

                if (n2a == end2) return false;
            }

            node_pointer start = n1;
            for(;n1 != end2; n1 = static_cast<node_pointer>(n1->next_))
            {
                value_type const& v = n1->value();
                if (find(start, n1, v)) continue;
                std::size_t matches = count_equal(n2, end2, v);
                if (!matches || matches != 1 + count_equal(
                        static_cast<node_pointer>(n1->next_), end1, v))
                    return false;
            }
            
            return true;
        }

        static bool find(node_pointer n, node_pointer end, value_type const& v)
        {
            for(;n != end; n = static_cast<node_pointer>(n->next_))
                if (n->value() == v)
                    return true;
            return false;
        }

        static std::size_t count_equal(node_pointer n, node_pointer end,
            value_type const& v)
        {
            std::size_t count = 0;
            for(;n != end; n = static_cast<node_pointer>(n->next_))
                if (n->value() == v) ++count;
            return count;
        }

#else

        static bool group_equals(node_pointer n1, node_pointer end1,
                node_pointer n2, node_pointer end2)
        {
            for(;;)
            {
                if(!extractor::compare_mapped(
                    n1->value(), n2->value()))
                    return false;

                n1 = static_cast<node_pointer>(n1->next_);
                n2 = static_cast<node_pointer>(n2->next_);

                if (n1 == end1) return n2 == end2;
                if (n2 == end2) return false;
            }
        }

#endif

        // Emplace/Insert

        static inline void add_after_node(
                node_pointer n,
                node_pointer pos)
        {
            n->next_ = static_cast<node_pointer>(pos->group_prev_)->next_;
            n->group_prev_ = pos->group_prev_;
            static_cast<node_pointer>(pos->group_prev_)->next_ =
                static_cast<link_pointer>(n);
            pos->group_prev_ = static_cast<link_pointer>(n);
        }

        inline node_pointer add_node(
                node_constructor& a,
                std::size_t hash,
                node_pointer pos)
        {
            node_pointer n = a.release();
            n->hash_ = hash;
            if(pos) {
                this->add_after_node(n, pos);
                if (n->next_) {
                    std::size_t next_bucket =
                        static_cast<node_pointer>(n->next_)->hash_ %
                        this->bucket_count_;
                    if (next_bucket != hash % this->bucket_count_) {
                        this->get_bucket(next_bucket)->next_ = n;
                    }
                }
            }
            else {
                bucket_pointer b = this->get_bucket(hash % this->bucket_count_);

                if (!b->next_)
                {
                    previous_pointer start_node = this->get_previous_start();
                    
                    if (start_node->next_) {
                        this->get_bucket(
                            static_cast<node_pointer>(start_node->next_)->hash_
                                % this->bucket_count_)->next_ = n;
                    }
    
                    b->next_ = start_node;
                    n->next_ = start_node->next_;
                    start_node->next_ = static_cast<link_pointer>(n);
                }
                else
                {
                    n->next_ = b->next_->next_;
                    b->next_->next_ = static_cast<link_pointer>(n);
                }
            }
            ++this->size_;
            return n;
        }

        node_pointer emplace_impl(node_constructor& a)
        {
            key_type const& k = this->get_key(a.value());
            std::size_t hash = this->hash_function()(k);
            node_pointer position = this->find_node(hash, k);

            // reserve has basic exception safety if the hash function
            // throws, strong otherwise.
            this->reserve_for_insert(this->size_ + 1);
            return this->add_node(a, hash, position);
        }

        void emplace_impl_no_rehash(node_constructor& a)
        {
            key_type const& k = this->get_key(a.value());
            std::size_t hash = this->hash_function()(k);
            this->add_node(a, hash,
                this->find_node(hash, k));
        }

#if defined(BOOST_NO_RVALUE_REFERENCES)
        iterator emplace(boost::unordered::detail::emplace_args1<
                boost::unordered::detail::please_ignore_this_overload> const&)
        {
            BOOST_ASSERT(false);
            return iterator();
        }
#endif

        template <BOOST_UNORDERED_EMPLACE_TEMPLATE>
        iterator emplace(BOOST_UNORDERED_EMPLACE_ARGS)
        {
            node_constructor a(this->node_alloc());
            a.construct_node();
            a.construct_value(BOOST_UNORDERED_EMPLACE_FORWARD);

            return iterator(emplace_impl(a));
        }

        ////////////////////////////////////////////////////////////////////////
        // Insert range methods

        // if hash function throws, or inserting > 1 element, basic exception
        // safety. Strong otherwise
        template <class I>
        typename boost::unordered::detail::enable_if_forward<I, void>::type
            insert_range(I i, I j)
        {
            if(i == j) return;

            std::size_t distance = boost::unordered::detail::distance(i, j);
            if(distance == 1) {
                node_constructor a(this->node_alloc());
                a.construct_node();
                a.construct_value2(*i);
                emplace_impl(a);
            }
            else {
                // Only require basic exception safety here
                this->reserve_for_insert(this->size_ + distance);

                node_constructor a(this->node_alloc());
                for (; i != j; ++i) {
                    a.construct_node();
                    a.construct_value2(*i);
                    emplace_impl_no_rehash(a);
                }
            }
        }

        template <class I>
        typename boost::unordered::detail::disable_if_forward<I, void>::type
            insert_range(I i, I j)
        {
            node_constructor a(this->node_alloc());
            for (; i != j; ++i) {
                a.construct_node();
                a.construct_value2(*i);
                emplace_impl(a);
            }
        }

        ////////////////////////////////////////////////////////////////////////
        // Erase
        //
        // no throw

        std::size_t erase_key(key_type const& k)
        {
            if(!this->size_) return 0;

            std::size_t hash = this->hash_function()(k);
            std::size_t bucket_index = hash % this->bucket_count_;
            bucket_pointer bucket = this->get_bucket(bucket_index);

            previous_pointer prev = bucket->next_;
            if (!prev) return 0;

            for (;;)
            {
                if (!prev->next_) return 0;
                std::size_t node_hash =
                    static_cast<node_pointer>(prev->next_)->hash_;
                if (node_hash % this->bucket_count_ != bucket_index)
                    return 0;
                if (node_hash == hash &&
                    this->key_eq()(k, this->get_key(
                        static_cast<node_pointer>(prev->next_)->value())))
                    break;
                prev = static_cast<previous_pointer>(
                    static_cast<node_pointer>(prev->next_)->group_prev_);
            }

            node_pointer pos = static_cast<node_pointer>(prev->next_);
            link_pointer end1 =
                static_cast<node_pointer>(pos->group_prev_)->next_;
            node_pointer end = static_cast<node_pointer>(end1);
            prev->next_ = end1;
            this->fix_buckets(bucket, prev, end);
            return this->delete_nodes(pos, end);
        }

        node_pointer erase(node_pointer r)
        {
            BOOST_ASSERT(r);
            node_pointer next = static_cast<node_pointer>(r->next_);

            bucket_pointer bucket = this->get_bucket(
                r->hash_ % this->bucket_count_);
            previous_pointer prev = unlink_node(*bucket, r);

            this->fix_buckets(bucket, prev, next);

            this->delete_node(r);

            return next;
        }

        node_pointer erase_range(node_pointer r1, node_pointer r2)
        {
            if (r1 == r2) return r2;

            std::size_t bucket_index = r1->hash_ % this->bucket_count_;
            previous_pointer prev = unlink_nodes(
                *this->get_bucket(bucket_index), r1, r2);
            this->fix_buckets_range(bucket_index, prev, r1, r2);
            this->delete_nodes(r1, r2);

            return r2;
        }

        static previous_pointer unlink_node(bucket& b, node_pointer n)
        {
            node_pointer next = static_cast<node_pointer>(n->next_);
            previous_pointer prev =
                static_cast<previous_pointer>(n->group_prev_);

            if(prev->next_ != n) {
                // The node is at the beginning of a group.

                // Find the previous node pointer:
                prev = b.next_;
                while(prev->next_ != n) {
                    prev = static_cast<previous_pointer>(
                        static_cast<node_pointer>(prev->next_)->group_prev_);
                }

                // Remove from group
                if (next && next->group_prev_ == static_cast<link_pointer>(n))
                {
                    next->group_prev_ = n->group_prev_;
                }
            }
            else if (next && next->group_prev_ == static_cast<link_pointer>(n))
            {
                // The deleted node is not at the end of the group, so
                // change the link from the next node.
                next->group_prev_ = n->group_prev_;
            }
            else {
                // The deleted node is at the end of the group, so the
                // first node in the group is pointing to it.
                // Find that to change its pointer.
                node_pointer x = static_cast<node_pointer>(n->group_prev_);
                while(x->group_prev_ != static_cast<link_pointer>(n)) {
                    x = static_cast<node_pointer>(x->group_prev_);
                }
                x->group_prev_ = n->group_prev_;
            }

            prev->next_ = static_cast<link_pointer>(next);
            return prev;
        }

        static previous_pointer unlink_nodes(bucket& b,
                node_pointer begin, node_pointer end)
        {
            previous_pointer prev = static_cast<previous_pointer>(
                begin->group_prev_);

            if(prev->next_ != static_cast<link_pointer>(begin)) {
                // The node is at the beginning of a group.

                // Find the previous node pointer:
                prev = b.next_;
                while(prev->next_ != static_cast<link_pointer>(begin))
                    prev = static_cast<previous_pointer>(
                        static_cast<node_pointer>(prev->next_)->group_prev_);

                if (end) split_group(end);
            }
            else {
                node_pointer group1 = split_group(begin);

                if (end) {
                    node_pointer group2 = split_group(end);

                    if(begin == group2) {
                        link_pointer end1 = group1->group_prev_;
                        link_pointer end2 = group2->group_prev_;
                        group1->group_prev_ = end2;
                        group2->group_prev_ = end1;
                    }
                }
            }

            prev->next_ = static_cast<link_pointer>(end);

            return prev;
        }

        // Break a ciruclar list into two, with split as the beginning
        // of the second group (if split is at the beginning then don't
        // split).
        static node_pointer split_group(node_pointer split)
        {
            // Find first node in group.
            node_pointer first = split;
            while (static_cast<node_pointer>(first->group_prev_)->next_ ==
                    static_cast<link_pointer>(first))
                first = static_cast<node_pointer>(first->group_prev_);

            if(first == split) return split;

            link_pointer last = first->group_prev_;
            first->group_prev_ = split->group_prev_;
            split->group_prev_ = last;

            return first;
        }

        ////////////////////////////////////////////////////////////////////////
        // copy_buckets_to
        //
        // Basic exception safety. If an exception is thrown this will
        // leave dst partially filled and the buckets unset.

        static void copy_buckets_to(buckets const& src, buckets& dst)
        {
            BOOST_ASSERT(!dst.buckets_);

            dst.create_buckets();

            node_constructor a(dst.node_alloc());

            node_pointer n = src.get_start();
            previous_pointer prev = dst.get_previous_start();

            while(n) {
                std::size_t hash = n->hash_;
                node_pointer group_end =
                    static_cast<node_pointer>(
                        static_cast<node_pointer>(n->group_prev_)->next_);

                a.construct_node();
                a.construct_value2(n->value());

                node_pointer first_node = a.release();
                node_pointer end = first_node;
                first_node->hash_ = hash;
                prev->next_ = static_cast<link_pointer>(first_node);
                ++dst.size_;

                for(n = static_cast<node_pointer>(n->next_); n != group_end;
                        n = static_cast<node_pointer>(n->next_))
                {
                    a.construct_node();
                    a.construct_value2(n->value());
                    end = a.release();
                    end->hash_ = hash;
                    add_after_node(end, first_node);
                    ++dst.size_;
                }

                prev = place_in_bucket(dst, prev, end);
            }
        }

        ////////////////////////////////////////////////////////////////////////
        // move_buckets_to
        //
        // Basic exception safety. The source nodes are left in an unusable
        // state if an exception throws.

        static void move_buckets_to(buckets& src, buckets& dst)
        {
            BOOST_ASSERT(!dst.buckets_);

            dst.create_buckets();

            node_constructor a(dst.node_alloc());

            node_pointer n = src.get_start();
            previous_pointer prev = dst.get_previous_start();

            while(n) {
                std::size_t hash = n->hash_;
                node_pointer group_end =
                    static_cast<node_pointer>(
                        static_cast<node_pointer>(n->group_prev_)->next_);

                a.construct_node();
                a.construct_value2(boost::move(n->value()));

                node_pointer first_node = a.release();
                node_pointer end = first_node;
                first_node->hash_ = hash;
                prev->next_ = static_cast<link_pointer>(first_node);
                ++dst.size_;

                for(n = static_cast<node_pointer>(n->next_); n != group_end;
                        n = static_cast<node_pointer>(n->next_))
                {
                    a.construct_node();
                    a.construct_value2(boost::move(n->value()));
                    end = a.release();
                    end->hash_ = hash;
                    add_after_node(end, first_node);
                    ++dst.size_;
                }

                prev = place_in_bucket(dst, prev, end);
            }
        }

        // strong otherwise exception safety
        void rehash_impl(std::size_t num_buckets)
        {
            BOOST_ASSERT(this->size_);

            buckets dst(this->node_alloc(), num_buckets);
            dst.create_buckets();

            previous_pointer src_start = this->get_previous_start();
            previous_pointer dst_start = dst.get_previous_start();

            dst_start->next_ = src_start->next_;
            src_start->next_ = link_pointer();
            dst.size_ = this->size_;
            this->size_ = 0;

            previous_pointer prev = dst_start;
            while (prev->next_)
                prev = place_in_bucket(dst, prev,
                    static_cast<node_pointer>(
                        static_cast<node_pointer>(prev->next_)->group_prev_));

            // Swap the new nodes back into the container and setup the
            // variables.
            dst.swap(*this); // no throw
        }

        // Iterate through the nodes placing them in the correct buckets.
        // pre: prev->next_ is not null.
        static previous_pointer place_in_bucket(buckets& dst,
                previous_pointer prev, node_pointer end)
        {
            bucket_pointer b = dst.get_bucket(end->hash_ % dst.bucket_count_);

            if (!b->next_) {
                b->next_ = static_cast<node_pointer>(prev);
                return static_cast<previous_pointer>(end);
            }
            else {
                link_pointer next = end->next_;
                end->next_ = b->next_->next_;
                b->next_->next_ = prev->next_;
                prev->next_ = next;
                return prev;
            }
        }
    };
}}}

#endif
