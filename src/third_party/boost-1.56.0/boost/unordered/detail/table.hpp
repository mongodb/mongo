
// Copyright (C) 2003-2004 Jeremy B. Maitin-Shepard.
// Copyright (C) 2005-2011 Daniel James
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_ALL_HPP_INCLUDED
#define BOOST_UNORDERED_DETAIL_ALL_HPP_INCLUDED

#include <boost/config.hpp>
#if defined(BOOST_HAS_PRAGMA_ONCE)
#pragma once
#endif

#include <boost/unordered/detail/buckets.hpp>
#include <boost/unordered/detail/util.hpp>
#include <boost/type_traits/aligned_storage.hpp>
#include <boost/type_traits/alignment_of.hpp>
#include <cmath>

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable:4127) // conditional expression is constant
#endif

#if defined(BOOST_UNORDERED_DEPRECATED_EQUALITY)

#if defined(__EDG__)
#elif defined(_MSC_VER) || defined(__BORLANDC__) || defined(__DMC__)
#pragma message("Warning: BOOST_UNORDERED_DEPRECATED_EQUALITY is no longer supported.")
#elif defined(__GNUC__) || defined(__HP_aCC) || \
    defined(__SUNPRO_CC) || defined(__IBMCPP__)
#warning "BOOST_UNORDERED_DEPRECATED_EQUALITY is no longer supported."
#endif

#endif

namespace boost { namespace unordered { namespace detail {

    ////////////////////////////////////////////////////////////////////////////
    // convert double to std::size_t

    inline std::size_t double_to_size(double f)
    {
        return f >= static_cast<double>(
            (std::numeric_limits<std::size_t>::max)()) ?
            (std::numeric_limits<std::size_t>::max)() :
            static_cast<std::size_t>(f);
    }

    // The space used to store values in a node.

    template <typename ValueType>
    struct value_base
    {
        typedef ValueType value_type;

        typename boost::aligned_storage<
            sizeof(value_type),
            boost::alignment_of<value_type>::value>::type data_;

        void* address() {
            return this;
        }

        value_type& value() {
            return *(ValueType*) this;
        }

        value_type* value_ptr() {
            return (ValueType*) this;
        }

    private:

        value_base& operator=(value_base const&);
    };

    template <typename NodeAlloc>
    struct copy_nodes
    {
        typedef boost::unordered::detail::allocator_traits<NodeAlloc>
            node_allocator_traits;

        node_constructor<NodeAlloc> constructor;

        explicit copy_nodes(NodeAlloc& a) : constructor(a) {}

        typename node_allocator_traits::pointer create(
                typename node_allocator_traits::value_type::value_type const& v)
        {
            constructor.construct_with_value2(v);
            return constructor.release();
        }
    };

    template <typename NodeAlloc>
    struct move_nodes
    {
        typedef boost::unordered::detail::allocator_traits<NodeAlloc>
            node_allocator_traits;

        node_constructor<NodeAlloc> constructor;

        explicit move_nodes(NodeAlloc& a) : constructor(a) {}

        typename node_allocator_traits::pointer create(
                typename node_allocator_traits::value_type::value_type& v)
        {
            constructor.construct_with_value2(boost::move(v));
            return constructor.release();
        }
    };

    template <typename Buckets>
    struct assign_nodes
    {
        node_holder<typename Buckets::node_allocator> holder;

        explicit assign_nodes(Buckets& b) : holder(b) {}

        typename Buckets::node_pointer create(
                typename Buckets::value_type const& v)
        {
            return holder.copy_of(v);
        }
    };

    template <typename Buckets>
    struct move_assign_nodes
    {
        node_holder<typename Buckets::node_allocator> holder;

        explicit move_assign_nodes(Buckets& b) : holder(b) {}

        typename Buckets::node_pointer create(
                typename Buckets::value_type& v)
        {
            return holder.move_copy_of(v);
        }
    };

    template <typename Types>
    struct table :
        boost::unordered::detail::functions<
            typename Types::hasher,
            typename Types::key_equal>
    {
    private:
        table(table const&);
        table& operator=(table const&);
    public:
        typedef typename Types::node node;
        typedef typename Types::bucket bucket;
        typedef typename Types::hasher hasher;
        typedef typename Types::key_equal key_equal;
        typedef typename Types::key_type key_type;
        typedef typename Types::extractor extractor;
        typedef typename Types::value_type value_type;
        typedef typename Types::table table_impl;
        typedef typename Types::link_pointer link_pointer;
        typedef typename Types::policy policy;

        typedef boost::unordered::detail::functions<
            typename Types::hasher,
            typename Types::key_equal> functions;
        typedef typename functions::set_hash_functions set_hash_functions;

        typedef typename Types::allocator allocator;
        typedef typename boost::unordered::detail::
            rebind_wrap<allocator, node>::type node_allocator;
        typedef typename boost::unordered::detail::
            rebind_wrap<allocator, bucket>::type bucket_allocator;
        typedef boost::unordered::detail::allocator_traits<node_allocator>
            node_allocator_traits;
        typedef boost::unordered::detail::allocator_traits<bucket_allocator>
            bucket_allocator_traits;
        typedef typename node_allocator_traits::pointer
            node_pointer;
        typedef typename node_allocator_traits::const_pointer
            const_node_pointer;
        typedef typename bucket_allocator_traits::pointer
            bucket_pointer;
        typedef boost::unordered::detail::node_constructor<node_allocator>
            node_constructor;

        typedef boost::unordered::iterator_detail::
            iterator<node> iterator;
        typedef boost::unordered::iterator_detail::
            c_iterator<node, const_node_pointer> c_iterator;
        typedef boost::unordered::iterator_detail::
            l_iterator<node, policy> l_iterator;
        typedef boost::unordered::iterator_detail::
            cl_iterator<node, const_node_pointer, policy> cl_iterator;

        ////////////////////////////////////////////////////////////////////////
        // Members

        boost::unordered::detail::compressed<bucket_allocator, node_allocator>
            allocators_;
        std::size_t bucket_count_;
        std::size_t size_;
        float mlf_;
        std::size_t max_load_;
        bucket_pointer buckets_;

        ////////////////////////////////////////////////////////////////////////
        // Data access

        bucket_allocator const& bucket_alloc() const
        {
            return allocators_.first();
        }

        node_allocator const& node_alloc() const
        {
            return allocators_.second();
        }

        bucket_allocator& bucket_alloc()
        {
            return allocators_.first();
        }

        node_allocator& node_alloc()
        {
            return allocators_.second();
        }

        std::size_t max_bucket_count() const
        {
            // -1 to account for the start bucket.
            return policy::prev_bucket_count(
                bucket_allocator_traits::max_size(bucket_alloc()) - 1);
        }

        bucket_pointer get_bucket(std::size_t bucket_index) const
        {
            BOOST_ASSERT(buckets_);
            return buckets_ + static_cast<std::ptrdiff_t>(bucket_index);
        }

        link_pointer get_previous_start() const
        {
            return get_bucket(bucket_count_)->first_from_start();
        }

        link_pointer get_previous_start(std::size_t bucket_index) const
        {
            return get_bucket(bucket_index)->next_;
        }

        iterator begin() const
        {
            return size_ ? iterator(get_previous_start()->next_) : iterator();
        }

        iterator begin(std::size_t bucket_index) const
        {
            if (!size_) return iterator();
            link_pointer prev = get_previous_start(bucket_index);
            return prev ? iterator(prev->next_) : iterator();
        }
        
        std::size_t hash_to_bucket(std::size_t hash_value) const
        {
            return policy::to_bucket(bucket_count_, hash_value);
        }

        float load_factor() const
        {
            BOOST_ASSERT(bucket_count_ != 0);
            return static_cast<float>(size_)
                / static_cast<float>(bucket_count_);
        }

        std::size_t bucket_size(std::size_t index) const
        {
            iterator it = begin(index);
            if (!it.node_) return 0;

            std::size_t count = 0;
            while(it.node_ && hash_to_bucket(it.node_->hash_) == index)
            {
                ++count;
                ++it;
            }

            return count;
        }

        ////////////////////////////////////////////////////////////////////////
        // Load methods

        std::size_t max_size() const
        {
            using namespace std;
    
            // size < mlf_ * count
            return boost::unordered::detail::double_to_size(ceil(
                    static_cast<double>(mlf_) *
                    static_cast<double>(max_bucket_count())
                )) - 1;
        }

        void recalculate_max_load()
        {
            using namespace std;
    
            // From 6.3.1/13:
            // Only resize when size >= mlf_ * count
            max_load_ = buckets_ ? boost::unordered::detail::double_to_size(ceil(
                    static_cast<double>(mlf_) *
                    static_cast<double>(bucket_count_)
                )) : 0;

        }

        void max_load_factor(float z)
        {
            BOOST_ASSERT(z > 0);
            mlf_ = (std::max)(z, minimum_max_load_factor);
            recalculate_max_load();
        }

        std::size_t min_buckets_for_size(std::size_t size) const
        {
            BOOST_ASSERT(mlf_ >= minimum_max_load_factor);
    
            using namespace std;
    
            // From 6.3.1/13:
            // size < mlf_ * count
            // => count > size / mlf_
            //
            // Or from rehash post-condition:
            // count > size / mlf_

            return policy::new_bucket_count(
                boost::unordered::detail::double_to_size(floor(
                    static_cast<double>(size) /
                    static_cast<double>(mlf_))) + 1);
        }

        ////////////////////////////////////////////////////////////////////////
        // Constructors

        table(std::size_t num_buckets,
                hasher const& hf,
                key_equal const& eq,
                node_allocator const& a) :
            functions(hf, eq),
            allocators_(a,a),
            bucket_count_(policy::new_bucket_count(num_buckets)),
            size_(0),
            mlf_(1.0f),
            max_load_(0),
            buckets_()
        {}

        table(table const& x, node_allocator const& a) :
            functions(x),
            allocators_(a,a),
            bucket_count_(x.min_buckets_for_size(x.size_)),
            size_(0),
            mlf_(x.mlf_),
            max_load_(0),
            buckets_()
        {}

        table(table& x, boost::unordered::detail::move_tag m) :
            functions(x, m),
            allocators_(x.allocators_, m),
            bucket_count_(x.bucket_count_),
            size_(x.size_),
            mlf_(x.mlf_),
            max_load_(x.max_load_),
            buckets_(x.buckets_)
        {
            x.buckets_ = bucket_pointer();
            x.size_ = 0;
            x.max_load_ = 0;
        }

        table(table& x, node_allocator const& a,
                boost::unordered::detail::move_tag m) :
            functions(x, m),
            allocators_(a, a),
            bucket_count_(x.bucket_count_),
            size_(0),
            mlf_(x.mlf_),
            max_load_(x.max_load_),
            buckets_()
        {}

        ////////////////////////////////////////////////////////////////////////
        // Initialisation.

        void init(table const& x)
        {
            if (x.size_) {
                create_buckets(bucket_count_);
                copy_nodes<node_allocator> node_creator(node_alloc());
                table_impl::fill_buckets(x.begin(), *this, node_creator);
            }
        }

        void move_init(table& x)
        {
            if(node_alloc() == x.node_alloc()) {
                move_buckets_from(x);
            }
            else if(x.size_) {
                // TODO: Could pick new bucket size?
                create_buckets(bucket_count_);

                move_nodes<node_allocator> node_creator(node_alloc());
                node_holder<node_allocator> nodes(x);
                table_impl::fill_buckets(nodes.begin(), *this, node_creator);
            }
        }

        ////////////////////////////////////////////////////////////////////////
        // Create buckets

        void create_buckets(std::size_t new_count)
        {
            boost::unordered::detail::array_constructor<bucket_allocator>
                constructor(bucket_alloc());
    
            // Creates an extra bucket to act as the start node.
            constructor.construct(bucket(), new_count + 1);

            if (buckets_)
            {
                // Copy the nodes to the new buckets, including the dummy
                // node if there is one.
                (constructor.get() +
                    static_cast<std::ptrdiff_t>(new_count))->next_ =
                        (buckets_ + static_cast<std::ptrdiff_t>(
                            bucket_count_))->next_;
                destroy_buckets();
            }
            else if (bucket::extra_node)
            {
                node_constructor a(node_alloc());
                a.construct();

                (constructor.get() +
                    static_cast<std::ptrdiff_t>(new_count))->next_ =
                        a.release();
            }

            bucket_count_ = new_count;
            buckets_ = constructor.release();
            recalculate_max_load();
        }

        ////////////////////////////////////////////////////////////////////////
        // Swap and Move

        void swap_allocators(table& other, false_type)
        {
            boost::unordered::detail::func::ignore_unused_variable_warning(other);

            // According to 23.2.1.8, if propagate_on_container_swap is
            // false the behaviour is undefined unless the allocators
            // are equal.
            BOOST_ASSERT(node_alloc() == other.node_alloc());
        }

        void swap_allocators(table& other, true_type)
        {
            allocators_.swap(other.allocators_);
        }

        // Only swaps the allocators if propagate_on_container_swap
        void swap(table& x)
        {
            set_hash_functions op1(*this, x);
            set_hash_functions op2(x, *this);

            // I think swap can throw if Propagate::value,
            // since the allocators' swap can throw. Not sure though.
            swap_allocators(x,
                boost::unordered::detail::integral_constant<bool,
                    allocator_traits<node_allocator>::
                    propagate_on_container_swap::value>());

            boost::swap(buckets_, x.buckets_);
            boost::swap(bucket_count_, x.bucket_count_);
            boost::swap(size_, x.size_);
            std::swap(mlf_, x.mlf_);
            std::swap(max_load_, x.max_load_);
            op1.commit();
            op2.commit();
        }

        void move_buckets_from(table& other)
        {
            BOOST_ASSERT(node_alloc() == other.node_alloc());
            BOOST_ASSERT(!buckets_);
            buckets_ = other.buckets_;
            bucket_count_ = other.bucket_count_;
            size_ = other.size_;
            other.buckets_ = bucket_pointer();
            other.size_ = 0;
            other.max_load_ = 0;
        }

        ////////////////////////////////////////////////////////////////////////
        // Delete/destruct

        ~table()
        {
            delete_buckets();
        }

        void delete_node(link_pointer prev)
        {
            node_pointer n = static_cast<node_pointer>(prev->next_);
            prev->next_ = n->next_;

            boost::unordered::detail::func::destroy_value_impl(node_alloc(),
                n->value_ptr());
            boost::unordered::detail::func::destroy(boost::addressof(*n));
            node_allocator_traits::deallocate(node_alloc(), n, 1);
            --size_;
        }

        std::size_t delete_nodes(link_pointer prev, link_pointer end)
        {
            BOOST_ASSERT(prev->next_ != end);

            std::size_t count = 0;

            do {
                delete_node(prev);
                ++count;
            } while (prev->next_ != end);

            return count;
        }

        void delete_buckets()
        {
            if(buckets_) {
                if (size_) delete_nodes(get_previous_start(), link_pointer());

                if (bucket::extra_node) {
                    node_pointer n = static_cast<node_pointer>(
                            get_bucket(bucket_count_)->next_);
                    boost::unordered::detail::func::destroy(
                            boost::addressof(*n));
                    node_allocator_traits::deallocate(node_alloc(), n, 1);
                }

                destroy_buckets();
                buckets_ = bucket_pointer();
                max_load_ = 0;
            }

            BOOST_ASSERT(!size_);
        }

        void clear()
        {
            if (!size_) return;

            delete_nodes(get_previous_start(), link_pointer());
            clear_buckets();

            BOOST_ASSERT(!size_);
        }

        void clear_buckets()
        {
            bucket_pointer end = get_bucket(bucket_count_);
            for(bucket_pointer it = buckets_; it != end; ++it)
            {
                it->next_ = node_pointer();
            }
        }

        void destroy_buckets()
        {
            bucket_pointer end = get_bucket(bucket_count_ + 1);
            for(bucket_pointer it = buckets_; it != end; ++it)
            {
                boost::unordered::detail::func::destroy(
                    boost::addressof(*it));
            }

            bucket_allocator_traits::deallocate(bucket_alloc(),
                buckets_, bucket_count_ + 1);
        }

        ////////////////////////////////////////////////////////////////////////
        // Fix buckets after delete
        //

        std::size_t fix_bucket(std::size_t bucket_index, link_pointer prev)
        {
            link_pointer end = prev->next_;
            std::size_t bucket_index2 = bucket_index;

            if (end)
            {
                bucket_index2 = hash_to_bucket(
                    static_cast<node_pointer>(end)->hash_);

                // If begin and end are in the same bucket, then
                // there's nothing to do.
                if (bucket_index == bucket_index2) return bucket_index2;

                // Update the bucket containing end.
                get_bucket(bucket_index2)->next_ = prev;
            }

            // Check if this bucket is now empty.
            bucket_pointer this_bucket = get_bucket(bucket_index);
            if (this_bucket->next_ == prev)
                this_bucket->next_ = link_pointer();

            return bucket_index2;
        }

        ////////////////////////////////////////////////////////////////////////
        // Assignment

        void assign(table const& x)
        {
            if (this != boost::addressof(x))
            {
                assign(x,
                    boost::unordered::detail::integral_constant<bool,
                        allocator_traits<node_allocator>::
                        propagate_on_container_copy_assignment::value>());
            }
        }

        void assign(table const& x, false_type)
        {
            // Strong exception safety.
            set_hash_functions new_func_this(*this, x);
            new_func_this.commit();
            mlf_ = x.mlf_;
            recalculate_max_load();

            if (!size_ && !x.size_) return;

            if (x.size_ >= max_load_) {
                create_buckets(min_buckets_for_size(x.size_));
            }
            else {
                clear_buckets();
            }

            // assign_nodes takes ownership of the container's elements,
            // assigning to them if possible, and deleting any that are
            // left over.
            assign_nodes<table> node_creator(*this);
            table_impl::fill_buckets(x.begin(), *this, node_creator);
        }

        void assign(table const& x, true_type)
        {
            if (node_alloc() == x.node_alloc()) {
                allocators_.assign(x.allocators_);
                assign(x, false_type());
            }
            else {
                set_hash_functions new_func_this(*this, x);

                // Delete everything with current allocators before assigning
                // the new ones.
                delete_buckets();
                allocators_.assign(x.allocators_);

                // Copy over other data, all no throw.
                new_func_this.commit();
                mlf_ = x.mlf_;
                bucket_count_ = min_buckets_for_size(x.size_);
                max_load_ = 0;

                // Finally copy the elements.
                if (x.size_) {
                    create_buckets(bucket_count_);
                    copy_nodes<node_allocator> node_creator(node_alloc());
                    table_impl::fill_buckets(x.begin(), *this, node_creator);
                }
            }
        }

        void move_assign(table& x)
        {
            if (this != boost::addressof(x))
            {
                move_assign(x,
                    boost::unordered::detail::integral_constant<bool,
                        allocator_traits<node_allocator>::
                        propagate_on_container_move_assignment::value>());
            }
        }

        void move_assign(table& x, true_type)
        {
            delete_buckets();
            allocators_.move_assign(x.allocators_);
            move_assign_no_alloc(x);
        }

        void move_assign(table& x, false_type)
        {
            if (node_alloc() == x.node_alloc()) {
                delete_buckets();
                move_assign_no_alloc(x);
            }
            else {
                set_hash_functions new_func_this(*this, x);
                new_func_this.commit();
                mlf_ = x.mlf_;
                recalculate_max_load();

                if (!size_ && !x.size_) return;

                if (x.size_ >= max_load_) {
                    create_buckets(min_buckets_for_size(x.size_));
                }
                else {
                    clear_buckets();
                }

                // move_assign_nodes takes ownership of the container's
                // elements, assigning to them if possible, and deleting
                // any that are left over.
                move_assign_nodes<table> node_creator(*this);
                node_holder<node_allocator> nodes(x);
                table_impl::fill_buckets(nodes.begin(), *this, node_creator);
            }
        }
        
        void move_assign_no_alloc(table& x)
        {
            set_hash_functions new_func_this(*this, x);
            // No throw from here.
            mlf_ = x.mlf_;
            max_load_ = x.max_load_;
            move_buckets_from(x);
            new_func_this.commit();
        }

        // Accessors

        key_type const& get_key(value_type const& x) const
        {
            return extractor::extract(x);
        }

        std::size_t hash(key_type const& k) const
        {
            return policy::apply_hash(this->hash_function(), k);
        }

        // Find Node

        template <typename Key, typename Hash, typename Pred>
        iterator generic_find_node(
                Key const& k,
                Hash const& hf,
                Pred const& eq) const
        {
            return static_cast<table_impl const*>(this)->
                find_node_impl(policy::apply_hash(hf, k), k, eq);
        }

        iterator find_node(
                std::size_t key_hash,
                key_type const& k) const
        {
            return static_cast<table_impl const*>(this)->
                find_node_impl(key_hash, k, this->key_eq());
        }

        iterator find_node(key_type const& k) const
        {
            return static_cast<table_impl const*>(this)->
                find_node_impl(hash(k), k, this->key_eq());
        }

        iterator find_matching_node(iterator n) const
        {
            // TODO: Does this apply to C++11?
            //
            // For some stupid reason, I decided to support equality comparison
            // when different hash functions are used. So I can't use the hash
            // value from the node here.
    
            return find_node(get_key(*n));
        }

        // Reserve and rehash

        void reserve_for_insert(std::size_t);
        void rehash(std::size_t);
        void reserve(std::size_t);
    };

    ////////////////////////////////////////////////////////////////////////////
    // Reserve & Rehash

    // basic exception safety
    template <typename Types>
    inline void table<Types>::reserve_for_insert(std::size_t size)
    {
        if (!buckets_) {
            create_buckets((std::max)(bucket_count_,
                min_buckets_for_size(size)));
        }
        // According to the standard this should be 'size >= max_load_',
        // but I think this is better, defect report filed.
        else if(size > max_load_) {
            std::size_t num_buckets
                = min_buckets_for_size((std::max)(size,
                    size_ + (size_ >> 1)));

            if (num_buckets != bucket_count_)
                static_cast<table_impl*>(this)->rehash_impl(num_buckets);
        }
    }

    // if hash function throws, basic exception safety
    // strong otherwise.

    template <typename Types>
    inline void table<Types>::rehash(std::size_t min_buckets)
    {
        using namespace std;

        if(!size_) {
            delete_buckets();
            bucket_count_ = policy::new_bucket_count(min_buckets);
        }
        else {
            min_buckets = policy::new_bucket_count((std::max)(min_buckets,
                boost::unordered::detail::double_to_size(floor(
                    static_cast<double>(size_) /
                    static_cast<double>(mlf_))) + 1));

            if(min_buckets != bucket_count_)
                static_cast<table_impl*>(this)->rehash_impl(min_buckets);
        }
    }

    template <typename Types>
    inline void table<Types>::reserve(std::size_t num_elements)
    {
        rehash(static_cast<std::size_t>(
            std::ceil(static_cast<double>(num_elements) / mlf_)));
    }
}}}

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

#endif
