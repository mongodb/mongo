
// Copyright (C) 2003-2004 Jeremy B. Maitin-Shepard.
// Copyright (C) 2005-2011 Daniel James
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_MANAGER_HPP_INCLUDED
#define BOOST_UNORDERED_DETAIL_MANAGER_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/unordered/detail/util.hpp>
#include <boost/unordered/detail/allocator_helpers.hpp>
#include <boost/unordered/detail/emplace_args.hpp>
#include <boost/type_traits/aligned_storage.hpp>
#include <boost/type_traits/alignment_of.hpp>
#include <boost/swap.hpp>
#include <boost/assert.hpp>

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable:4127) // conditional expression is constant
#endif

namespace boost { namespace unordered { namespace detail {

    template <typename Types> struct table;
    template <typename NodePointer> struct bucket;
    struct ptr_bucket;
    template <typename A, typename Bucket, typename Node> struct buckets;

    ///////////////////////////////////////////////////////////////////
    //
    // Node construction

    template <typename NodeAlloc>
    struct node_constructor
    {
    private:

        typedef NodeAlloc node_allocator;
        typedef boost::unordered::detail::allocator_traits<NodeAlloc>
            node_allocator_traits;
        typedef typename node_allocator_traits::value_type node;
        typedef typename node_allocator_traits::pointer node_pointer;
        typedef typename node::value_type value_type;

        node_allocator& alloc_;
        node_pointer node_;
        bool node_constructed_;
        bool value_constructed_;

    public:

        node_constructor(node_allocator& n) :
            alloc_(n),
            node_(),
            node_constructed_(false),
            value_constructed_(false)
        {
        }

        ~node_constructor();

        void construct_node();

        template <BOOST_UNORDERED_EMPLACE_TEMPLATE>
        void construct_value(BOOST_UNORDERED_EMPLACE_ARGS)
        {
            BOOST_ASSERT(node_ && node_constructed_ && !value_constructed_);
            boost::unordered::detail::construct_impl(
                node_->value_ptr(), BOOST_UNORDERED_EMPLACE_FORWARD);
            value_constructed_ = true;
        }

        template <typename A0>
        void construct_value2(BOOST_FWD_REF(A0) a0)
        {
            BOOST_ASSERT(node_ && node_constructed_ && !value_constructed_);
            boost::unordered::detail::construct_impl2(
                node_->value_ptr(), boost::forward<A0>(a0));
            value_constructed_ = true;
        }

        value_type const& value() const {
            BOOST_ASSERT(node_ && node_constructed_ && value_constructed_);
            return node_->value();
        }

        // no throw
        node_pointer release()
        {
            node_pointer p = node_;
            node_ = node_pointer();
            return p;
        }

    private:
        node_constructor(node_constructor const&);
        node_constructor& operator=(node_constructor const&);
    };
    
    template <typename Alloc>
    node_constructor<Alloc>::~node_constructor()
    {
        if (node_) {
            if (value_constructed_) {
                boost::unordered::detail::destroy(node_->value_ptr());
            }

            if (node_constructed_) {
                node_allocator_traits::destroy(alloc_,
                    boost::addressof(*node_));
            }

            node_allocator_traits::deallocate(alloc_, node_, 1);
        }
    }

    template <typename Alloc>
    void node_constructor<Alloc>::construct_node()
    {
        if(!node_) {
            node_constructed_ = false;
            value_constructed_ = false;

            node_ = node_allocator_traits::allocate(alloc_, 1);

            node_allocator_traits::construct(alloc_,
                boost::addressof(*node_), node());
            node_->init(static_cast<typename node::link_pointer>(node_));
            node_constructed_ = true;
        }
        else {
            BOOST_ASSERT(node_constructed_);

            if (value_constructed_)
            {
                boost::unordered::detail::destroy(node_->value_ptr());
                value_constructed_ = false;
            }
        }
    }

    ///////////////////////////////////////////////////////////////////
    //
    // Bucket

    template <typename NodePointer>
    struct bucket
    {
        typedef NodePointer previous_pointer;
        previous_pointer next_;

        bucket() : next_() {}

        previous_pointer first_from_start()
        {
            return next_;
        }

        enum { extra_node = true };
    };

    struct ptr_bucket
    {
        typedef ptr_bucket* previous_pointer;
        previous_pointer next_;

        ptr_bucket() : next_(0) {}

        previous_pointer first_from_start()
        {
            return this;
        }

        enum { extra_node = false };
    };

    ///////////////////////////////////////////////////////////////////
    //
    // Buckets

    template <typename A, typename Bucket, typename Node>
    struct buckets
    {
    private:
        buckets(buckets const&);
        buckets& operator=(buckets const&);
    public:
        typedef boost::unordered::detail::allocator_traits<A> traits;
        typedef typename traits::value_type value_type;

        typedef Node node;
        typedef Bucket bucket;
        typedef typename boost::unordered::detail::rebind_wrap<A, node>::type
            node_allocator;
        typedef typename boost::unordered::detail::rebind_wrap<A, bucket>::type
            bucket_allocator;
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
        typedef typename bucket::previous_pointer
            previous_pointer;
        typedef boost::unordered::detail::node_constructor<node_allocator>
            node_constructor;

        // Members

        bucket_pointer buckets_;
        std::size_t bucket_count_;
        std::size_t size_;
        boost::unordered::detail::compressed<bucket_allocator, node_allocator>
            allocators_;

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
            return boost::unordered::detail::prev_prime(
                bucket_allocator_traits::max_size(bucket_alloc()) - 1);
        }

        bucket_pointer get_bucket(std::size_t bucket_index) const
        {
            return buckets_ + static_cast<std::ptrdiff_t>(bucket_index);
        }

        previous_pointer get_previous_start() const
        {
            return this->get_bucket(this->bucket_count_)->first_from_start();
        }

        previous_pointer get_previous_start(std::size_t bucket_index) const
        {
            return this->get_bucket(bucket_index)->next_;
        }

        node_pointer get_start() const
        {
            return static_cast<node_pointer>(this->get_previous_start()->next_);
        }

        node_pointer get_start(std::size_t bucket_index) const
        {
            previous_pointer prev = this->get_previous_start(bucket_index);
            return prev ? static_cast<node_pointer>(prev->next_) :
                node_pointer();
        }

        float load_factor() const
        {
            BOOST_ASSERT(this->bucket_count_ != 0);
            return static_cast<float>(this->size_)
                / static_cast<float>(this->bucket_count_);
        }

        std::size_t bucket_size(std::size_t index) const
        {
            if (!this->size_) return 0;
            node_pointer ptr = this->get_start(index);
            if (!ptr) return 0;

            std::size_t count = 0;
            while(ptr && ptr->hash_ % this->bucket_count_ == index)
            {
                ++count;
                ptr = static_cast<node_pointer>(ptr->next_);
            }

            return count;
        }

        ////////////////////////////////////////////////////////////////////////
        // Constructors

        buckets(node_allocator const& a, std::size_t bucket_count) :
            buckets_(),
            bucket_count_(bucket_count),
            size_(),
            allocators_(a,a)
        {
        }

        buckets(buckets& b, boost::unordered::detail::move_tag m) :
            buckets_(),
            bucket_count_(b.bucket_count_),
            size_(),
            allocators_(b.allocators_, m)
        {
            swap(b);
        }

        template <typename Types>
        buckets(boost::unordered::detail::table<Types>& x,
                boost::unordered::detail::move_tag m) :
            buckets_(),
            bucket_count_(x.bucket_count_),
            size_(),
            allocators_(x.allocators_, m)
        {
            swap(x);
        }

        ////////////////////////////////////////////////////////////////////////
        // Create buckets
        // (never called in constructor to avoid exception issues)

        void create_buckets()
        {
            boost::unordered::detail::array_constructor<bucket_allocator>
                constructor(bucket_alloc());
    
            // Creates an extra bucket to act as the start node.
            constructor.construct(bucket(), this->bucket_count_ + 1);
    
            if (bucket::extra_node)
            {
                node_constructor a(this->node_alloc());
                a.construct_node();

                (constructor.get() +
                    static_cast<std::ptrdiff_t>(this->bucket_count_))->next_ =
                        a.release();
            }

            this->buckets_ = constructor.release();
        }

        ////////////////////////////////////////////////////////////////////////
        // Swap and Move

        void swap(buckets& other, false_type = false_type())
        {
            BOOST_ASSERT(node_alloc() == other.node_alloc());
            boost::swap(buckets_, other.buckets_);
            boost::swap(bucket_count_, other.bucket_count_);
            boost::swap(size_, other.size_);
        }

        void swap(buckets& other, true_type)
        {
            allocators_.swap(other.allocators_);
            boost::swap(buckets_, other.buckets_);
            boost::swap(bucket_count_, other.bucket_count_);
            boost::swap(size_, other.size_);
        }

        void move_buckets_from(buckets& other)
        {
            BOOST_ASSERT(node_alloc() == other.node_alloc());
            BOOST_ASSERT(!this->buckets_);
            this->buckets_ = other.buckets_;
            this->bucket_count_ = other.bucket_count_;
            this->size_ = other.size_;
            other.buckets_ = bucket_pointer();
            other.bucket_count_ = 0;
            other.size_ = 0;
        }

        ////////////////////////////////////////////////////////////////////////
        // Delete/destruct

        inline void delete_node(node_pointer n)
        {
            boost::unordered::detail::destroy(n->value_ptr());
            node_allocator_traits::destroy(node_alloc(), boost::addressof(*n));
            node_allocator_traits::deallocate(node_alloc(), n, 1);
            --size_;
        }

        std::size_t delete_nodes(node_pointer begin, node_pointer end)
        {
            std::size_t count = 0;

            while(begin != end) {
                node_pointer n = begin;
                begin = static_cast<node_pointer>(begin->next_);
                delete_node(n);
                ++count;
            }

            return count;
        }

        inline void delete_extra_node(bucket_pointer) {}

        inline void delete_extra_node(node_pointer n) {
            node_allocator_traits::destroy(node_alloc(), boost::addressof(*n));
            node_allocator_traits::deallocate(node_alloc(), n, 1);
        }

        inline ~buckets()
        {
            this->delete_buckets();
        }

        void delete_buckets()
        {
            if(this->buckets_) {
                previous_pointer prev = this->get_previous_start();

                while(prev->next_) {
                    node_pointer n = static_cast<node_pointer>(prev->next_);
                    prev->next_ = n->next_;
                    delete_node(n);
                }

                delete_extra_node(prev);

                bucket_pointer end = this->get_bucket(this->bucket_count_ + 1);
                for(bucket_pointer it = this->buckets_; it != end; ++it)
                {
                    bucket_allocator_traits::destroy(bucket_alloc(),
                        boost::addressof(*it));
                }

                bucket_allocator_traits::deallocate(bucket_alloc(),
                    this->buckets_, this->bucket_count_ + 1);
    
                this->buckets_ = bucket_pointer();
            }

            BOOST_ASSERT(!this->size_);
        }

        void clear()
        {
            if(!this->size_) return;

            previous_pointer prev = this->get_previous_start();

            while(prev->next_) {
                node_pointer n = static_cast<node_pointer>(prev->next_);
                prev->next_ = n->next_;
                delete_node(n);
            }

            bucket_pointer end = this->get_bucket(this->bucket_count_);
            for(bucket_pointer it = this->buckets_; it != end; ++it)
            {
                it->next_ = node_pointer();
            }

            BOOST_ASSERT(!this->size_);
        }

        // This is called after erasing a node or group of nodes to fix up
        // the bucket pointers.
        void fix_buckets(bucket_pointer bucket,
                previous_pointer prev, node_pointer next)
        {
            if (!next)
            {
                if (bucket->next_ == prev) bucket->next_ = node_pointer();
            }
            else
            {
                bucket_pointer next_bucket = this->get_bucket(
                    next->hash_ % this->bucket_count_);

                if (next_bucket != bucket)
                {
                    next_bucket->next_ = prev;
                    if (bucket->next_ == prev) bucket->next_ = node_pointer();
                }
            }
        }

        // This is called after erasing a range of nodes to fix any bucket
        // pointers into that range.
        void fix_buckets_range(std::size_t bucket_index,
                previous_pointer prev, node_pointer begin, node_pointer end)
        {
            node_pointer n = begin;
    
            // If we're not at the start of the current bucket, then
            // go to the start of the next bucket.
            if (this->get_bucket(bucket_index)->next_ != prev)
            {
                for(;;) {
                    n = static_cast<node_pointer>(n->next_);
                    if (n == end) return;
    
                    std::size_t new_bucket_index =
                        n->hash_ % this->bucket_count_;
                    if (bucket_index != new_bucket_index) {
                        bucket_index = new_bucket_index;
                        break;
                    }
                }
            }
    
            // Iterate through the remaining nodes, clearing out the bucket
            // pointers.
            this->get_bucket(bucket_index)->next_ = previous_pointer();
            for(;;) {
                n = static_cast<node_pointer>(n->next_);
                if (n == end) break;
    
                std::size_t new_bucket_index =
                    n->hash_ % this->bucket_count_;
                if (bucket_index != new_bucket_index) {
                    bucket_index = new_bucket_index;
                    this->get_bucket(bucket_index)->next_ = previous_pointer();
                }
            };
    
            // Finally fix the bucket containing the trailing node.
            if (n) {
                this->get_bucket(n->hash_ % this->bucket_count_)->next_
                    = prev;
            }
        }
    };

    ////////////////////////////////////////////////////////////////////////////
    // Functions

    // Assigning and swapping the equality and hash function objects
    // needs strong exception safety. To implement that normally we'd
    // require one of them to be known to not throw and the other to
    // guarantee strong exception safety. Unfortunately they both only
    // have basic exception safety. So to acheive strong exception
    // safety we have storage space for two copies, and assign the new
    // copies to the unused space. Then switch to using that to use
    // them. This is implemented in 'set_hash_functions' which
    // atomically assigns the new function objects in a strongly
    // exception safe manner.

    template <class H, class P> class set_hash_functions;

    template <class H, class P>
    class functions
    {
        friend class boost::unordered::detail::set_hash_functions<H, P>;
        functions& operator=(functions const&);

        typedef compressed<H, P> function_pair;

        typedef typename boost::aligned_storage<
            sizeof(function_pair),
            boost::alignment_of<function_pair>::value>::type aligned_function;

        bool current_; // The currently active functions.
        aligned_function funcs_[2];

        function_pair const& current() const {
            return *static_cast<function_pair const*>(
                static_cast<void const*>(&funcs_[current_]));
        }

        void construct(bool which, H const& hf, P const& eq)
        {
            new((void*) &funcs_[which]) function_pair(hf, eq);
        }

        void construct(bool which, function_pair const& f)
        {
            new((void*) &funcs_[which]) function_pair(f);
        }
        
        void destroy(bool which)
        {
            boost::unordered::detail::destroy((function_pair*)(&funcs_[which]));
        }
        
    public:

        functions(H const& hf, P const& eq)
            : current_(false)
        {
            construct(current_, hf, eq);
        }

        functions(functions const& bf)
            : current_(false)
        {
            construct(current_, bf.current());
        }

        ~functions() {
            this->destroy(current_);
        }

        H const& hash_function() const {
            return current().first();
        }

        P const& key_eq() const {
            return current().second();
        }
    };
    
    template <class H, class P>
    class set_hash_functions
    {
        set_hash_functions(set_hash_functions const&);
        set_hash_functions& operator=(set_hash_functions const&);
    
        functions<H,P>& functions_;
        bool tmp_functions_;

    public:

        set_hash_functions(functions<H,P>& f, H const& h, P const& p)
          : functions_(f),
            tmp_functions_(!f.current_)
        {
            f.construct(tmp_functions_, h, p);
        }

        set_hash_functions(functions<H,P>& f, functions<H,P> const& other)
          : functions_(f),
            tmp_functions_(!f.current_)
        {
            f.construct(tmp_functions_, other.current());
        }

        ~set_hash_functions()
        {
            functions_.destroy(tmp_functions_);
        }

        void commit()
        {
            functions_.current_ = tmp_functions_;
            tmp_functions_ = !tmp_functions_;
        }
    };
}}}

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

#endif
