/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   attribute_value_set.cpp
 * \author Andrey Semashev
 * \date   19.04.2007
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <new>
#include <memory>
#include <boost/array.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/derivation_value_traits.hpp>
#include <boost/log/attributes/attribute_name.hpp>
#include <boost/log/attributes/attribute_value.hpp>
#include <boost/log/attributes/attribute_value_set.hpp>
#include "alignment_gap_between.hpp"
#include "attribute_set_impl.hpp"
#include "stateless_allocator.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

BOOST_FORCEINLINE attribute_value_set::node_base::node_base() :
    m_pPrev(NULL),
    m_pNext(NULL)
{
}

BOOST_FORCEINLINE attribute_value_set::node::node(key_type const& key, mapped_type& data, bool dynamic) :
    node_base(),
    m_Value(key, mapped_type()),
    m_DynamicallyAllocated(dynamic)
{
    m_Value.second.swap(data);
}

//! Container implementation
struct attribute_value_set::implementation
{
public:
    typedef key_type::id_type id_type;

private:
    typedef attribute_set::implementation attribute_set_impl_type;
    typedef boost::log::aux::stateless_allocator< char > stateless_allocator;

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

    //! A container that provides iteration through elements of the container
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

    //! Element disposer
    struct disposer
    {
        typedef void result_type;
        void operator() (node* p) const BOOST_NOEXCEPT
        {
            if (!p->m_DynamicallyAllocated)
                p->~node();
            else
                delete p;
        }
    };

private:
    //! Pointer to the source-specific attributes
    attribute_set_impl_type* m_pSourceAttributes;
    //! Pointer to the thread-specific attributes
    attribute_set_impl_type* m_pThreadAttributes;
    //! Pointer to the global attributes
    attribute_set_impl_type* m_pGlobalAttributes;

    //! The container with elements
    node_list m_Nodes;
    //! The pointer to the end of the allocated elements within the storage
    node* m_pEnd;
    //! The pointer to the end of storage
    node* m_pEOS;

    //! Hash table buckets
    buckets m_Buckets;

private:
    //! Constructor
    implementation(
        node* storage,
        node* eos,
        attribute_set_impl_type* source_attrs,
        attribute_set_impl_type* thread_attrs,
        attribute_set_impl_type* global_attrs
    ) :
        m_pSourceAttributes(source_attrs),
        m_pThreadAttributes(thread_attrs),
        m_pGlobalAttributes(global_attrs),
        m_pEnd(storage),
        m_pEOS(eos)
    {
    }

    //! Destructor
    ~implementation()
    {
        m_Nodes.clear_and_dispose(disposer());
    }

    //! The function allocates memory and creates the object
    static implementation* create(
        size_type element_count,
        attribute_set_impl_type* source_attrs,
        attribute_set_impl_type* thread_attrs,
        attribute_set_impl_type* global_attrs)
    {
        // Calculate the buffer size
        const size_type header_size = sizeof(implementation) +
            aux::alignment_gap_between< implementation, node >::value;
        const size_type buffer_size = header_size + element_count * sizeof(node);

        implementation* p = reinterpret_cast< implementation* >(stateless_allocator().allocate(buffer_size));
        node* const storage = reinterpret_cast< node* >(reinterpret_cast< char* >(p) + header_size);
        new (p) implementation(storage, storage + element_count, source_attrs, thread_attrs, global_attrs);

        return p;
    }

public:
    //! The function allocates memory and creates the object
    static implementation* create(
        attribute_set const& source_attrs,
        attribute_set const& thread_attrs,
        attribute_set const& global_attrs,
        size_type reserve_count)
    {
        return create(
            source_attrs.m_pImpl->size() + thread_attrs.m_pImpl->size() + global_attrs.m_pImpl->size() + reserve_count,
            source_attrs.m_pImpl,
            thread_attrs.m_pImpl,
            global_attrs.m_pImpl);
    }

    //! The function allocates memory and creates the object
    static implementation* create(
        attribute_value_set const& source_attrs,
        attribute_set const& thread_attrs,
        attribute_set const& global_attrs,
        size_type reserve_count)
    {
        implementation* p = create(
            source_attrs.m_pImpl->size() + thread_attrs.m_pImpl->size() + global_attrs.m_pImpl->size() + reserve_count,
            NULL,
            thread_attrs.m_pImpl,
            global_attrs.m_pImpl);
        p->copy_nodes_from(source_attrs.m_pImpl);
        return p;
    }

    //! The function allocates memory and creates the object
    static implementation* create(
        BOOST_RV_REF(attribute_value_set) source_attrs,
        attribute_set const& thread_attrs,
        attribute_set const& global_attrs,
        size_type reserve_count)
    {
        implementation* p = source_attrs.m_pImpl;
        source_attrs.m_pImpl = NULL;
        p->m_pThreadAttributes = thread_attrs.m_pImpl;
        p->m_pGlobalAttributes = global_attrs.m_pImpl;
        return p;
    }

    //! The function allocates memory and creates the object
    static implementation* create(size_type reserve_count)
    {
        return create(reserve_count, NULL, NULL, NULL);
    }

    //! Creates a copy of the object
    static implementation* copy(implementation* that)
    {
        // Create new object
        implementation* p = create(that->size(), NULL, NULL, NULL);

        // Copy all elements
        p->copy_nodes_from(that);

        return p;
    }

    //! Destroys the object and releases the memory
    static void destroy(implementation* p)
    {
        const size_type buffer_size = reinterpret_cast< char* >(p->m_pEOS) - reinterpret_cast< char* >(p);
        p->~implementation();
        stateless_allocator().deallocate(reinterpret_cast< stateless_allocator::pointer >(p), buffer_size);
    }

    //! Returns the pointer to the first element
    node_base* begin()
    {
        freeze();
        return m_Nodes.begin().pointed_node();
    }
    //! Returns the pointer after the last element
    node_base* end()
    {
        return m_Nodes.end().pointed_node();
    }

    //! Returns the number of elements in the container
    size_type size()
    {
        freeze();
        return m_Nodes.size();
    }

    //! Looks for the element with an equivalent key
    node_base* find(key_type key)
    {
        // First try to find an acquired element
        bucket& b = get_bucket(key.id());
        node* p = b.first;
        if (p)
        {
            // The bucket is not empty, search among the elements
            p = find_in_bucket(key, b);
            if (p->m_Value.first == key)
                return p;
        }

        // Element not found, try to acquire the value from attribute sets
        return freeze_node(key, b, p);
    }

    //! Freezes all elements of the container
    void freeze()
    {
        if (m_pSourceAttributes)
        {
            freeze_nodes_from(m_pSourceAttributes);
            m_pSourceAttributes = NULL;
        }
        if (m_pThreadAttributes)
        {
            freeze_nodes_from(m_pThreadAttributes);
            m_pThreadAttributes = NULL;
        }
        if (m_pGlobalAttributes)
        {
            freeze_nodes_from(m_pGlobalAttributes);
            m_pGlobalAttributes = NULL;
        }
    }

    //! Inserts an element
    std::pair< node*, bool > insert(key_type key, mapped_type const& mapped)
    {
        bucket& b = get_bucket(key.id());
        node* p = find_in_bucket(key, b);
        if (!p || p->m_Value.first != key)
        {
            p = insert_node(key, b, p, mapped);
            return std::pair< node*, bool >(p, true);
        }
        else
        {
            return std::pair< node*, bool >(p, false);
        }
    }

private:
    //! The function returns a bucket for the specified element
    bucket& get_bucket(id_type id)
    {
        return m_Buckets[id & (buckets::static_size - 1u)];
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

    //! Acquires the attribute value from the attribute sets
    node_base* freeze_node(key_type key, bucket& b, node* where)
    {
        attribute_set::iterator it;
        if (m_pSourceAttributes)
        {
            it = m_pSourceAttributes->find(key);
            if (it != m_pSourceAttributes->end())
            {
                // The attribute is found, acquiring the value
                return insert_node(key, b, where, it->second.get_value());
            }
        }

        if (m_pThreadAttributes)
        {
            it = m_pThreadAttributes->find(key);
            if (it != m_pThreadAttributes->end())
            {
                // The attribute is found, acquiring the value
                return insert_node(key, b, where, it->second.get_value());
            }
        }

        if (m_pGlobalAttributes)
        {
            it = m_pGlobalAttributes->find(key);
            if (it != m_pGlobalAttributes->end())
            {
                // The attribute is found, acquiring the value
                return insert_node(key, b, where, it->second.get_value());
            }
        }

        // The attribute is not found
        return m_Nodes.end().pointed_node();
    }

    //! The function inserts a node into the container
    node* insert_node(key_type key, bucket& b, node* where, mapped_type data)
    {
        node* p;
        if (m_pEnd != m_pEOS)
        {
            p = m_pEnd++;
            new (p) node(key, data, false);
        }
        else
        {
            p = new node(key, data, true);
        }

        node_list::iterator it;
        if (b.first == NULL)
        {
            // The bucket is empty
            b.first = b.last = p;
            it = m_Nodes.end();
        }
        else if (where == b.last && key.id() > where->m_Value.first.id())
        {
            // The new element should become the last element of the bucket
            it = m_Nodes.iterator_to(*where);
            ++it;
            b.last = p;
        }
        else if (where == b.first)
        {
            // The new element should become the first element of the bucket
            it = m_Nodes.iterator_to(*where);
            b.first = p;
        }
        else
        {
            // The new element should be within the bucket
            it = m_Nodes.iterator_to(*where);
        }

        m_Nodes.insert(it, *p);

        return p;
    }

    //! Acquires attribute values from the set of attributes
    void freeze_nodes_from(attribute_set_impl_type* attrs)
    {
        attribute_set::const_iterator it = attrs->begin(), end = attrs->end();
        for (; it != end; ++it)
        {
            key_type key = it->first;
            bucket& b = get_bucket(key.id());
            node* p = b.first;
            if (p)
            {
                p = find_in_bucket(key, b);
                if (p->m_Value.first == key)
                    continue; // the element is already frozen
            }

            insert_node(key, b, p, it->second.get_value());
        }
    }

    //! Copies nodes of the container
    void copy_nodes_from(implementation* from)
    {
        // Copy all elements
        node_list::iterator it = from->m_Nodes.begin(), end = from->m_Nodes.end();
        for (; it != end; ++it)
        {
            node* n = m_pEnd++;
            mapped_type data = it->m_Value.second;
            new (n) node(it->m_Value.first, data, false);
            m_Nodes.push_back(*n);

            // Since nodes within buckets are ordered, we can simply append the node to the end of the bucket
            bucket& b = get_bucket(n->m_Value.first.id());
            if (b.first == NULL)
                b.first = b.last = n;
            else
                b.last = n;
        }
    }
};

//! The constructor creates an empty set
BOOST_LOG_API attribute_value_set::attribute_value_set(
    size_type reserve_count
) :
    m_pImpl(implementation::create(reserve_count))
{
}

//! The constructor adopts three attribute sets to the set
BOOST_LOG_API attribute_value_set::attribute_value_set(
    attribute_set const& source_attrs,
    attribute_set const& thread_attrs,
    attribute_set const& global_attrs,
    size_type reserve_count
) :
    m_pImpl(implementation::create(source_attrs, thread_attrs, global_attrs, reserve_count))
{
}

//! The constructor adopts three attribute sets to the set
BOOST_LOG_API attribute_value_set::attribute_value_set(
    attribute_value_set const& source_attrs,
    attribute_set const& thread_attrs,
    attribute_set const& global_attrs,
    size_type reserve_count
) :
    m_pImpl(implementation::create(source_attrs, thread_attrs, global_attrs, reserve_count))
{
}

//! The constructor adopts three attribute sets to the set
BOOST_LOG_API void attribute_value_set::construct(
    attribute_value_set& source_attrs,
    attribute_set const& thread_attrs,
    attribute_set const& global_attrs,
    size_type reserve_count
)
{
    m_pImpl = implementation::create(boost::move(source_attrs), thread_attrs, global_attrs, reserve_count);
}

//! Copy constructor
BOOST_LOG_API attribute_value_set::attribute_value_set(attribute_value_set const& that)
{
    if (that.m_pImpl)
        m_pImpl = implementation::copy(that.m_pImpl);
    else
        m_pImpl = NULL;
}

//! Destructor
BOOST_LOG_API attribute_value_set::~attribute_value_set() BOOST_NOEXCEPT
{
    if (m_pImpl)
    {
        implementation::destroy(m_pImpl);
        m_pImpl = NULL;
    }
}

//  Iterator generators
BOOST_LOG_API attribute_value_set::const_iterator
attribute_value_set::begin() const
{
    return const_iterator(m_pImpl->begin(), const_cast< attribute_value_set* >(this));
}

BOOST_LOG_API attribute_value_set::const_iterator
attribute_value_set::end() const
{
    return const_iterator(m_pImpl->end(), const_cast< attribute_value_set* >(this));
}

//! The method returns number of elements in the container
BOOST_LOG_API attribute_value_set::size_type
attribute_value_set::size() const
{
    return m_pImpl->size();
}

//! Internal lookup implementation
BOOST_LOG_API attribute_value_set::const_iterator
attribute_value_set::find(key_type key) const
{
    return const_iterator(m_pImpl->find(key), const_cast< attribute_value_set* >(this));
}

//! The method acquires values of all adopted attributes. Users don't need to call it, since will always get an already frozen set.
BOOST_LOG_API void attribute_value_set::freeze()
{
    m_pImpl->freeze();
}

//! Inserts an element into the set
BOOST_LOG_API std::pair< attribute_value_set::const_iterator, bool >
attribute_value_set::insert(key_type key, mapped_type const& mapped)
{
    std::pair< node*, bool > res = m_pImpl->insert(key, mapped);
    return std::pair< const_iterator, bool >(const_iterator(res.first, this), res.second);
}

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
