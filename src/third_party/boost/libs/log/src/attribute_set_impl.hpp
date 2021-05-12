/*
 *          Copyright Andrey Semashev 2007 - 2020.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   attribute_set_impl.hpp
 * \author Andrey Semashev
 * \date   03.07.2010
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_ATTRIBUTE_SET_IMPL_HPP_INCLUDED_
#define BOOST_LOG_ATTRIBUTE_SET_IMPL_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <new>
#include <memory>
#include <limits>
#include <utility>
#include <algorithm>
#include <cstddef>
#include <boost/assert.hpp>
#include <boost/array.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/derivation_value_traits.hpp>
#include <boost/log/attributes/attribute_set.hpp>
#include <boost/log/detail/allocator_traits.hpp>
#include <boost/log/detail/header.hpp>

#ifndef BOOST_LOG_HASH_TABLE_SIZE_LOG
// Hash table size will be 2 ^ this value
#define BOOST_LOG_HASH_TABLE_SIZE_LOG 4
#endif

#ifndef BOOST_LOG_ATTRIBUTE_SET_MAX_POOL_SIZE
// Maximum pool size that each attribute set maintains
#define BOOST_LOG_ATTRIBUTE_SET_MAX_POOL_SIZE 8
#endif

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

//! A simple pooling allocator
template< typename T >
class pool_allocator :
    public std::allocator< T >
{
public:
    template< typename U >
    struct rebind
    {
        typedef pool_allocator< U > other;
    };

    typedef std::allocator< T > base_type;

#if BOOST_LOG_ATTRIBUTE_SET_MAX_POOL_SIZE > 0

    typedef typename log::aux::allocator_traits< base_type >::value_type value_type;
    typedef typename log::aux::allocator_traits< base_type >::size_type size_type;
    typedef typename log::aux::allocator_traits< base_type >::difference_type difference_type;
    typedef typename log::aux::allocator_traits< base_type >::pointer pointer;
    typedef typename log::aux::allocator_traits< base_type >::const_pointer const_pointer;
    typedef value_type& reference;
    typedef value_type const& const_reference;

private:
    array< pointer, BOOST_LOG_ATTRIBUTE_SET_MAX_POOL_SIZE > m_Pool;
    size_type m_PooledCount;

public:
    pool_allocator() : m_PooledCount(0)
    {
    }

    pool_allocator(pool_allocator const& that) :
        base_type(static_cast< base_type const& >(that)),
        m_PooledCount(0)
    {
    }

    template< typename U >
    pool_allocator(pool_allocator< U > const& that) :
        base_type(static_cast< typename pool_allocator< U >::base_type const& >(that)),
        m_PooledCount(0)
    {
    }

    ~pool_allocator()
    {
        for (size_type i = 0; i < m_PooledCount; ++i)
        {
            log::aux::allocator_traits< base_type >::deallocate(*static_cast< base_type* >(this), m_Pool[i], 1);
        }
    }

    pool_allocator& operator= (pool_allocator const& that)
    {
        base_type::operator= (static_cast< base_type const& >(that));
        return *this;
    }

    template< typename U >
    pool_allocator& operator= (pool_allocator< U > const& that)
    {
        base_type::operator= (
            static_cast< typename pool_allocator< U >::base_type const& >(that));
        return *this;
    }

    pointer allocate(size_type n, const void* hint = NULL)
    {
        if (BOOST_LIKELY(m_PooledCount > 0))
        {
            --m_PooledCount;
            return m_Pool[m_PooledCount];
        }
        else
        {
            return log::aux::allocator_traits< base_type >::allocate(*static_cast< base_type* >(this), n, hint);
        }
    }

    void deallocate(pointer p, size_type n)
    {
        if (BOOST_LIKELY(m_PooledCount < m_Pool.size()))
        {
            m_Pool[m_PooledCount] = p;
            ++m_PooledCount;
        }
        else
        {
            log::aux::allocator_traits< base_type >::deallocate(*static_cast< base_type* >(this), p, n);
        }
    }

#else

    template< typename U >
    pool_allocator(pool_allocator< U > const& that) :
        base_type(static_cast< typename pool_allocator< U >::base_type const& >(that))
    {
    }

#endif // BOOST_LOG_ATTRIBUTE_SET_MAX_POOL_SIZE > 0
};

//! Attribute set implementation
struct attribute_set::implementation
{
public:
    //! Attribute name identifier type
    typedef key_type::id_type id_type;

    //! Allocator type
    typedef pool_allocator< node > node_allocator;

    //! Node base class traits for the intrusive list
    struct node_traits
    {
        typedef node_base node;
        typedef node* node_ptr;
        typedef node const* const_node_ptr;
        static node* get_next(const node* n) { return n->m_pNext; }
        static void set_next(node* n, node* next) { n->m_pNext = next; }
        static node* get_previous(const node* n) { return n->m_pPrev; }
        static void set_previous(node* n, node* prev) { n->m_pPrev = prev; }
    };

    //! Contained node traits for the intrusive list
    typedef intrusive::derivation_value_traits<
        node,
        node_traits,
        intrusive::normal_link
    > value_traits;

    //! The container that allows to iterate through elements
    typedef intrusive::list<
        node,
        intrusive::value_traits< value_traits >,
        intrusive::constant_time_size< true >
    > node_list;

    //! A hash table bucket
    struct bucket
    {
        //! Points to the first element in the bucket
        node* first;
        //! Points to the last element in the bucket (not the one after the last!)
        node* last;

        bucket() : first(NULL), last(NULL) {}
    };

    //! A list of buckets
    typedef boost::array< bucket, 1u << BOOST_LOG_HASH_TABLE_SIZE_LOG > buckets;

    //! Cleanup function object used to erase elements from the container
    struct disposer
    {
        typedef void result_type;

        explicit disposer(node_allocator& alloc) : m_Allocator(alloc)
        {
        }
        void operator() (node* p) const
        {
            p->~node();
            m_Allocator.deallocate(p, 1);
        }

    private:
        node_allocator& m_Allocator;
    };

private:
    //! List of nodes
    node_list m_Nodes;
    //! Node allocator
    node_allocator m_Allocator;
    //! Hash table buckets
    buckets m_Buckets;

public:
    implementation()
    {
    }

    implementation(implementation const& that) : m_Allocator(that.m_Allocator)
    {
        node_list::const_iterator it = that.m_Nodes.begin(), end = that.m_Nodes.end();
        for (; it != end; ++it)
        {
            node* const n = m_Allocator.allocate(1, NULL);
            new (n) node(it->m_Value.first, it->m_Value.second);
            m_Nodes.push_back(*n);

            bucket& b = get_bucket(it->m_Value.first.id());
            if (b.first == NULL)
                b.first = b.last = n;
            else
                b.last = n;
        }
    }

    ~implementation()
    {
        m_Nodes.clear_and_dispose(disposer(m_Allocator));
    }

    size_type size() const { return m_Nodes.size(); }
    iterator begin() { return iterator(m_Nodes.begin().pointed_node()); }
    iterator end() { return iterator(m_Nodes.end().pointed_node()); }

    void clear()
    {
        m_Nodes.clear_and_dispose(disposer(m_Allocator));
        std::fill_n(m_Buckets.begin(), m_Buckets.size(), bucket());
    }

    std::pair< iterator, bool > insert(key_type key, mapped_type const& data)
    {
        BOOST_ASSERT(!!key);

        bucket& b = get_bucket(key.id());
        node* p = b.first;
        if (p)
        {
            // The bucket is not empty, search among the elements
            p = find_in_bucket(key, b);
            if (p->m_Value.first == key)
                return std::make_pair(iterator(p), false);
        }

        node* const n = m_Allocator.allocate(1, NULL);
        new (n) node(key, data);

        node_list::iterator it;
        if (b.first == NULL)
        {
            // The bucket is empty
            b.first = b.last = n;
            it = m_Nodes.end();
        }
        else if (p == b.last && key.id() > p->m_Value.first.id())
        {
            // The new element should become the last element of the bucket
            it = m_Nodes.iterator_to(*p);
            ++it;
            b.last = n;
        }
        else if (p == b.first)
        {
            // The new element should become the first element of the bucket
            it = m_Nodes.iterator_to(*p);
            b.first = n;
        }
        else
        {
            // The new element should be within the bucket
            it = m_Nodes.iterator_to(*p);
        }

        m_Nodes.insert(it, *n);

        return std::make_pair(iterator(n), true);
    }

    void erase(iterator it)
    {
        typedef node_list::node_traits node_traits;
        typedef node_list::value_traits value_traits;

        node* p = static_cast< node* >(it.base());

        // Adjust bucket boundaries, if needed
        bucket& b = get_bucket(it->first.id());
        if (p == b.first)
        {
            if (p == b.last)
            {
                // The erased element is the only one in the bucket
                b.first = b.last = NULL;
            }
            else
            {
                // The erased element is the first one in the bucket
                b.first = value_traits::to_value_ptr(node_traits::get_next(b.first));
            }
        }
        else if (p == b.last)
        {
            // The erased element is the last one in the bucket
            b.last = value_traits::to_value_ptr(node_traits::get_previous(b.last));
        }

        m_Nodes.erase_and_dispose(m_Nodes.iterator_to(*p), disposer(m_Allocator));
    }

    iterator find(key_type key)
    {
        bucket& b = get_bucket(key.id());
        node* p = b.first;
        if (p)
        {
            // The bucket is not empty, search among the elements
            p = find_in_bucket(key, b);
            if (p->m_Value.first == key)
                return iterator(p);
        }

        return end();
    }

private:
    implementation& operator= (implementation const&);

    //! The function returns a bucket for the specified element
    bucket& get_bucket(id_type id)
    {
        return m_Buckets[id & (buckets::static_size - 1)];
    }

    //! Attempts to find an element with the specified key in the bucket
    node* find_in_bucket(key_type key, bucket const& b)
    {
        typedef node_list::node_traits node_traits;
        typedef node_list::value_traits value_traits;

        // All elements within the bucket are sorted to speedup the search.
        node* p = b.first;
        while (p != b.last && p->m_Value.first.id() < key.id())
        {
            p = value_traits::to_value_ptr(node_traits::get_next(p));
        }

        return p;
    }
};

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_ATTRIBUTE_SET_IMPL_HPP_INCLUDED_
