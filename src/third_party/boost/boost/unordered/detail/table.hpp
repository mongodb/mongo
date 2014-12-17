
// Copyright (C) 2003-2004 Jeremy B. Maitin-Shepard.
// Copyright (C) 2005-2011 Daniel James
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_ALL_HPP_INCLUDED
#define BOOST_UNORDERED_DETAIL_ALL_HPP_INCLUDED

#include <boost/unordered/detail/buckets.hpp>
#include <boost/unordered/detail/util.hpp>
#include <boost/type_traits/aligned_storage.hpp>
#include <boost/type_traits/alignment_of.hpp>
#include <boost/iterator.hpp>
#include <cmath>

namespace boost { namespace unordered { namespace iterator_detail {

    ////////////////////////////////////////////////////////////////////////////
    // Iterators
    //
    // all no throw

    template <typename NodePointer, typename Value> struct iterator;
    template <typename ConstNodePointer, typename NodePointer,
        typename Value> struct c_iterator;
    template <typename NodePointer, typename Value> struct l_iterator;
    template <typename ConstNodePointer, typename NodePointer,
        typename Value> struct cl_iterator;

    // Local Iterators
    //
    // all no throw

    template <typename NodePointer, typename Value>
    struct l_iterator
        : public boost::iterator<
            std::forward_iterator_tag, Value, std::ptrdiff_t,
            NodePointer, Value&>
    {
#if !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
        template <typename ConstNodePointer, typename NodePointer2,
                typename Value2>
        friend struct boost::unordered::iterator_detail::cl_iterator;
    private:
#endif
        typedef NodePointer node_pointer;
        node_pointer ptr_;
        std::size_t bucket_;
        std::size_t bucket_count_;

    public:

        l_iterator() : ptr_() {}

        l_iterator(node_pointer x, std::size_t b, std::size_t c)
            : ptr_(x), bucket_(b), bucket_count_(c) {}

        Value& operator*() const {
            return ptr_->value();
        }

        Value* operator->() const {
            return ptr_->value_ptr();
        }

        l_iterator& operator++() {
            ptr_ = static_cast<node_pointer>(ptr_->next_);
            if (ptr_ && ptr_->hash_ % bucket_count_ != bucket_)
                ptr_ = node_pointer();
            return *this;
        }

        l_iterator operator++(int) {
            l_iterator tmp(*this);
            ++(*this);
            return tmp;
        }

        bool operator==(l_iterator x) const {
            return ptr_ == x.ptr_;
        }

        bool operator!=(l_iterator x) const {
            return ptr_ != x.ptr_;
        }
    };

    template <typename ConstNodePointer, typename NodePointer, typename Value>
    struct cl_iterator
        : public boost::iterator<
            std::forward_iterator_tag, Value, std::ptrdiff_t,
            ConstNodePointer, Value const&>
    {
        friend struct boost::unordered::iterator_detail::l_iterator
            <NodePointer, Value>;
    private:

        typedef NodePointer node_pointer;
        node_pointer ptr_;
        std::size_t bucket_;
        std::size_t bucket_count_;

    public:

        cl_iterator() : ptr_() {}

        cl_iterator(node_pointer x, std::size_t b, std::size_t c) :
            ptr_(x), bucket_(b), bucket_count_(c) {}

        cl_iterator(boost::unordered::iterator_detail::l_iterator<
                NodePointer, Value> const& x) :
            ptr_(x.ptr_), bucket_(x.bucket_), bucket_count_(x.bucket_count_)
        {}

        Value const&
            operator*() const {
            return ptr_->value();
        }

        Value const* operator->() const {
            return ptr_->value_ptr();
        }

        cl_iterator& operator++() {
            ptr_ = static_cast<node_pointer>(ptr_->next_);
            if (ptr_ && ptr_->hash_ % bucket_count_ != bucket_)
                ptr_ = node_pointer();
            return *this;
        }

        cl_iterator operator++(int) {
            cl_iterator tmp(*this);
            ++(*this);
            return tmp;
        }

        friend bool operator==(cl_iterator const& x, cl_iterator const& y) {
            return x.ptr_ == y.ptr_;
        }

        friend bool operator!=(cl_iterator const& x, cl_iterator const& y) {
            return x.ptr_ != y.ptr_;
        }
    };

    template <typename NodePointer, typename Value>
    struct iterator
        : public boost::iterator<
            std::forward_iterator_tag, Value, std::ptrdiff_t,
            NodePointer, Value&>
    {
#if !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
        template <typename ConstNodePointer, typename NodePointer2,
                typename Value2>
        friend struct boost::unordered::iterator_detail::c_iterator;
    private:
#endif
        typedef NodePointer node_pointer;
        node_pointer node_;

    public:

        iterator() : node_() {}

        explicit iterator(node_pointer const& x) : node_(x) {}

        Value& operator*() const {
            return node_->value();
        }

        Value* operator->() const {
            return &node_->value();
        }

        iterator& operator++() {
            node_ = static_cast<node_pointer>(node_->next_);
            return *this;
        }

        iterator operator++(int) {
            iterator tmp(node_);
            node_ = static_cast<node_pointer>(node_->next_);
            return tmp;
        }

        bool operator==(iterator const& x) const {
            return node_ == x.node_;
        }

        bool operator!=(iterator const& x) const {
            return node_ != x.node_;
        }
    };

    template <typename ConstNodePointer, typename NodePointer, typename Value>
    struct c_iterator
        : public boost::iterator<
            std::forward_iterator_tag, Value, std::ptrdiff_t,
            ConstNodePointer, Value const&>
    {
        friend struct boost::unordered::iterator_detail::iterator<
                NodePointer, Value>;

#if !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
        template <typename K, typename T, typename H, typename P, typename A>
        friend class boost::unordered::unordered_map;
        template <typename K, typename T, typename H, typename P, typename A>
        friend class boost::unordered::unordered_multimap;
        template <typename T, typename H, typename P, typename A>
        friend class boost::unordered::unordered_set;
        template <typename T, typename H, typename P, typename A>
        friend class boost::unordered::unordered_multiset;

    private:
#endif

        typedef NodePointer node_pointer;
        node_pointer node_;

    public:

        c_iterator() : node_() {}

        explicit c_iterator(node_pointer const& x) : node_(x) {}

        c_iterator(boost::unordered::iterator_detail::iterator<
                NodePointer, Value> const& x) : node_(x.node_) {}

        Value const& operator*() const {
            return node_->value();
        }

        Value const* operator->() const {
            return &node_->value();
        }

        c_iterator& operator++() {
            node_ = static_cast<node_pointer>(node_->next_);
            return *this;
        }

        c_iterator operator++(int) {
            c_iterator tmp(node_);
            node_ = static_cast<node_pointer>(node_->next_);
            return tmp;
        }

        friend bool operator==(c_iterator const& x, c_iterator const& y) {
            return x.node_ == y.node_;
        }

        friend bool operator!=(c_iterator const& x, c_iterator const& y) {
            return x.node_ != y.node_;
        }
    };
}}}

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

    template <typename Types>
    struct table :
        boost::unordered::detail::buckets<
            typename Types::allocator,
            typename Types::bucket,
            typename Types::node>,
        boost::unordered::detail::functions<
            typename Types::hasher,
            typename Types::key_equal>
    {
    private:
        table(table const&);
        table& operator=(table const&);
    public:
        typedef typename Types::hasher hasher;
        typedef typename Types::key_equal key_equal;
        typedef typename Types::key_type key_type;
        typedef typename Types::extractor extractor;
        typedef typename Types::value_type value_type;
        typedef typename Types::table table_impl;
        typedef typename Types::link_pointer link_pointer;

        typedef boost::unordered::detail::functions<
            typename Types::hasher,
            typename Types::key_equal> functions;

        typedef boost::unordered::detail::buckets<
            typename Types::allocator,
            typename Types::bucket,
            typename Types::node> buckets;

        typedef typename buckets::node_allocator node_allocator;
        typedef typename buckets::node_allocator_traits node_allocator_traits;
        typedef typename buckets::node_pointer node_pointer;
        typedef typename buckets::const_node_pointer const_node_pointer;

        typedef boost::unordered::iterator_detail::
            iterator<node_pointer, value_type> iterator;
        typedef boost::unordered::iterator_detail::
            c_iterator<const_node_pointer, node_pointer, value_type> c_iterator;
        typedef boost::unordered::iterator_detail::
            l_iterator<node_pointer, value_type> l_iterator;
        typedef boost::unordered::iterator_detail::
            cl_iterator<const_node_pointer, node_pointer, value_type>
            cl_iterator;

        // Members

        float mlf_;
        std::size_t max_load_; // Only use if this->buckets_.

        ////////////////////////////////////////////////////////////////////////
        // Load methods

        std::size_t max_size() const
        {
            using namespace std;
    
            // size < mlf_ * count
            return boost::unordered::detail::double_to_size(ceil(
                    static_cast<double>(this->mlf_) *
                    static_cast<double>(this->max_bucket_count())
                )) - 1;
        }

        std::size_t calculate_max_load()
        {
            using namespace std;
    
            // From 6.3.1/13:
            // Only resize when size >= mlf_ * count
            return boost::unordered::detail::double_to_size(ceil(
                    static_cast<double>(this->mlf_) *
                    static_cast<double>(this->bucket_count_)
                ));

        }
        void max_load_factor(float z)
        {
            BOOST_ASSERT(z > 0);
            mlf_ = (std::max)(z, minimum_max_load_factor);
            if (this->buckets_)
                this->max_load_ = this->calculate_max_load();
        }

        std::size_t min_buckets_for_size(std::size_t size) const
        {
            BOOST_ASSERT(this->mlf_ != 0);
    
            using namespace std;
    
            // From 6.3.1/13:
            // size < mlf_ * count
            // => count > size / mlf_
            //
            // Or from rehash post-condition:
            // count > size / mlf_

            return boost::unordered::detail::next_prime(
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
            buckets(a, boost::unordered::detail::next_prime(num_buckets)),
            functions(hf, eq),
            mlf_(1.0f),
            max_load_(0)
        {}

        table(table const& x, node_allocator const& a) :
            buckets(a, x.min_buckets_for_size(x.size_)),
            functions(x),
            mlf_(x.mlf_),
            max_load_(0)
        {
            if(x.size_) {
                table_impl::copy_buckets_to(x, *this);
                this->max_load_ = calculate_max_load();
            }
        }

        // TODO: Why calculate_max_load?
        table(table& x, boost::unordered::detail::move_tag m) :
            buckets(x, m),
            functions(x),
            mlf_(x.mlf_),
            max_load_(calculate_max_load())
        {}

        // TODO: Why not calculate_max_load?
        // TODO: Why do I use x's bucket count?
        table(table& x, node_allocator const& a,
                boost::unordered::detail::move_tag m) :
            buckets(a, x.bucket_count_),
            functions(x),
            mlf_(x.mlf_),
            max_load_(x.max_load_)
        {
            if(a == x.node_alloc()) {
                this->buckets::swap(x, false_type());
            }
            else if(x.size_) {
                // Use a temporary table because move_buckets_to leaves the
                // source container in a complete mess.

                buckets tmp(x, m);
                table_impl::move_buckets_to(tmp, *this);
                this->max_load_ = calculate_max_load();
            }
        }

        // Iterators

        node_pointer begin() const {
            return !this->buckets_ ?
                node_pointer() : this->get_start();
        }

        // Assignment

        void assign(table const& x)
        {
            assign(x,
                boost::unordered::detail::integral_constant<bool,
                    allocator_traits<node_allocator>::
                    propagate_on_container_copy_assignment::value>());
        }

        void assign(table const& x, false_type)
        {
            table tmp(x, this->node_alloc());
            this->swap(tmp, false_type());
        }

        void assign(table const& x, true_type)
        {
            table tmp(x, x.node_alloc());
            // Need to delete before setting the allocator so that buckets
            // aren't deleted with the wrong allocator.
            if(this->buckets_) this->delete_buckets();
            // TODO: Can allocator assignment throw?
            this->allocators_.assign(x.allocators_);
            this->swap(tmp, false_type());
        }

        void move_assign(table& x)
        {
            move_assign(x,
                boost::unordered::detail::integral_constant<bool,
                    allocator_traits<node_allocator>::
                    propagate_on_container_move_assignment::value>());
        }

        void move_assign(table& x, true_type)
        {
            if(this->buckets_) this->delete_buckets();
            this->allocators_.move_assign(x.allocators_);
            move_assign_no_alloc(x);
        }

        void move_assign(table& x, false_type)
        {
            if(this->node_alloc() == x.node_alloc()) {
                if(this->buckets_) this->delete_buckets();
                move_assign_no_alloc(x);
            }
            else {
                boost::unordered::detail::set_hash_functions<hasher, key_equal>
                    new_func_this(*this, x);

                if (x.size_) {
                    buckets b(this->node_alloc(),
                        x.min_buckets_for_size(x.size_));
                    buckets tmp(x, move_tag());
                    table_impl::move_buckets_to(tmp, b);
                    b.swap(*this);
                }
                else {
                    this->clear();
                }
                
                this->mlf_ = x.mlf_;
                if (this->buckets_) this->max_load_ = calculate_max_load();
                new_func_this.commit();
            }
        }
        
        void move_assign_no_alloc(table& x)
        {
            boost::unordered::detail::set_hash_functions<hasher, key_equal>
                new_func_this(*this, x);
            // No throw from here.
            this->move_buckets_from(x);
            this->mlf_ = x.mlf_;
            this->max_load_ = x.max_load_;
            new_func_this.commit();
        }

        ////////////////////////////////////////////////////////////////////////
        // Swap & Move

        void swap(table& x)
        {
            swap(x,
                boost::unordered::detail::integral_constant<bool,
                    allocator_traits<node_allocator>::
                    propagate_on_container_swap::value>());
        }

        // Only swaps the allocators if Propagate::value
        template <typename Propagate>
        void swap(table& x, Propagate p)
        {
            boost::unordered::detail::set_hash_functions<hasher, key_equal>
                op1(*this, x);
            boost::unordered::detail::set_hash_functions<hasher, key_equal>
                op2(x, *this);
            // I think swap can throw if Propagate::value,
            // since the allocators' swap can throw. Not sure though.
            this->buckets::swap(x, p);
            std::swap(this->mlf_, x.mlf_);
            std::swap(this->max_load_, x.max_load_);
            op1.commit();
            op2.commit();
        }

        // Swap everything but the allocators, and the functions objects.
        void swap_contents(table& x)
        {
            this->buckets::swap(x, false_type());
            std::swap(this->mlf_, x.mlf_);
            std::swap(this->max_load_, x.max_load_);
        }

        // Accessors

        key_type const& get_key(value_type const& x) const
        {
            return extractor::extract(x);
        }

        // Find Node

        template <typename Key, typename Hash, typename Pred>
        node_pointer generic_find_node(
                Key const& k,
                Hash const& hash_function,
                Pred const& eq) const
        {
            if (!this->size_) return node_pointer();
            return static_cast<table_impl const*>(this)->
                find_node_impl(hash_function(k), k, eq);
        }

        node_pointer find_node(
                std::size_t hash,
                key_type const& k) const
        {
            if (!this->size_) return node_pointer();
            return static_cast<table_impl const*>(this)->
                find_node_impl(hash, k, this->key_eq());
        }

        node_pointer find_node(key_type const& k) const
        {
            if (!this->size_) return node_pointer();
            return static_cast<table_impl const*>(this)->
                find_node_impl(this->hash_function()(k), k, this->key_eq());
        }

        node_pointer find_matching_node(node_pointer n) const
        {
            // TODO: Does this apply to C++11?
            //
            // For some stupid reason, I decided to support equality comparison
            // when different hash functions are used. So I can't use the hash
            // value from the node here.
    
            return find_node(get_key(n->value()));
        }

        // Reserve and rehash

        void reserve_for_insert(std::size_t);
        void rehash(std::size_t);
    };

    ////////////////////////////////////////////////////////////////////////////
    // Reserve & Rehash

    // basic exception safety
    template <typename Types>
    inline void table<Types>::reserve_for_insert(std::size_t size)
    {
        if (!this->buckets_) {
            this->bucket_count_ = (std::max)(this->bucket_count_,
                this->min_buckets_for_size(size));
            this->create_buckets();
            this->max_load_ = this->calculate_max_load();
        }
        else if(size >= max_load_) {
            std::size_t num_buckets
                = this->min_buckets_for_size((std::max)(size,
                    this->size_ + (this->size_ >> 1)));
            if (num_buckets != this->bucket_count_) {
                static_cast<table_impl*>(this)->rehash_impl(num_buckets);
                this->max_load_ = this->calculate_max_load();
            }
        }
    }

    // if hash function throws, basic exception safety
    // strong otherwise.

    template <typename Types>
    void table<Types>::rehash(std::size_t min_buckets)
    {
        using namespace std;

        if(!this->size_) {
            if(this->buckets_) this->delete_buckets();
            this->bucket_count_ = next_prime(min_buckets);
        }
        else {
            min_buckets = next_prime((std::max)(min_buckets,
                boost::unordered::detail::double_to_size(floor(
                    static_cast<double>(this->size_) /
                    static_cast<double>(mlf_))) + 1));

            if(min_buckets != this->bucket_count_) {
                static_cast<table_impl*>(this)->rehash_impl(min_buckets);
                this->max_load_ = this->calculate_max_load();
            }
        }
    }
}}}

#endif
