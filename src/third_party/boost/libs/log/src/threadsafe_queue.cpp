/*
 *          Copyright Andrey Semashev 2007 - 2021.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   threadsafe_queue.cpp
 * \author Andrey Semashev
 * \date   05.11.2010
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 *
 * The implementation is based on algorithms published in the "Simple, Fast,
 * and Practical Non-Blocking and Blocking Concurrent Queue Algorithms" article
 * in PODC96 by Maged M. Michael and Michael L. Scott. Pseudocode is available here:
 * http://www.cs.rochester.edu/research/synchronization/pseudocode/queues.html
 *
 * The lock-free version of the mentioned algorithms contain a race condition and therefore
 * were not included here.
 */

#include <boost/log/detail/config.hpp>
#include <boost/log/detail/threadsafe_queue.hpp>

#ifndef BOOST_LOG_NO_THREADS

#include <new>
#include <boost/assert.hpp>
#include <boost/throw_exception.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/atomic.hpp>
#include <boost/align/aligned_alloc.hpp>
#include <boost/type_traits/alignment_of.hpp>
#include <boost/log/detail/adaptive_mutex.hpp>
#include <boost/log/detail/locks.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! Generic queue implementation with two locks
class threadsafe_queue_impl_generic :
    public threadsafe_queue_impl
{
private:
    //! Mutex type to be used
    typedef adaptive_mutex mutex_type;

    /*!
     * A structure that contains a pointer to the node and the associated mutex.
     * The alignment below allows to eliminate false sharing, it should not be less than CPU cache line size.
     */
    struct BOOST_ALIGNMENT(BOOST_LOG_CPU_CACHE_LINE_SIZE) pointer
    {
        //! Pointer to the either end of the queue
        node_base* node;
        //! Lock for access synchronization
        mutex_type mutex;
        //  128 bytes padding is chosen to mitigate false sharing for NetBurst CPUs, which load two cache lines in one go.
        unsigned char padding[128U - (sizeof(node_base*) + sizeof(mutex_type)) % 128U];
    };

private:
    //! Pointer to the beginning of the queue
    pointer m_Head;
    //! Pointer to the end of the queue
    pointer m_Tail;

public:
    explicit threadsafe_queue_impl_generic(node_base* first_node)
    {
        set_next(first_node, NULL);
        m_Head.node = m_Tail.node = first_node;
    }

    static void* operator new (std::size_t size)
    {
        void* p = alignment::aligned_alloc(BOOST_LOG_CPU_CACHE_LINE_SIZE, size);
        if (BOOST_UNLIKELY(!p))
            BOOST_THROW_EXCEPTION(std::bad_alloc());
        return p;
    }

    static void operator delete (void* p, std::size_t) BOOST_NOEXCEPT
    {
        alignment::aligned_free(p);
    }

    node_base* reset_last_node() BOOST_NOEXCEPT
    {
        BOOST_ASSERT(m_Head.node == m_Tail.node);
        node_base* p = m_Head.node;
        m_Head.node = m_Tail.node = NULL;
        return p;
    }

    bool unsafe_empty() const BOOST_NOEXCEPT
    {
        return m_Head.node == m_Tail.node;
    }

    void push(node_base* p)
    {
        set_next(p, NULL);
        exclusive_lock_guard< mutex_type > lock(m_Tail.mutex);
        set_next(m_Tail.node, p);
        m_Tail.node = p;
    }

    bool try_pop(node_base*& node_to_free, node_base*& node_with_value)
    {
        exclusive_lock_guard< mutex_type > lock(m_Head.mutex);
        node_base* next = get_next(m_Head.node);
        if (next)
        {
            // We have a node to pop
            node_to_free = m_Head.node;
            node_with_value = m_Head.node = next;
            return true;
        }
        else
            return false;
    }

private:
    BOOST_FORCEINLINE static void set_next(node_base* p, node_base* next)
    {
        p->next.store(next, boost::memory_order_relaxed);
    }
    BOOST_FORCEINLINE static node_base* get_next(node_base* p)
    {
        return p->next.load(boost::memory_order_relaxed);
    }

    // Copying and assignment are closed
    BOOST_DELETED_FUNCTION(threadsafe_queue_impl_generic(threadsafe_queue_impl_generic const&))
    BOOST_DELETED_FUNCTION(threadsafe_queue_impl_generic& operator= (threadsafe_queue_impl_generic const&))
};

inline threadsafe_queue_impl::threadsafe_queue_impl()
{
}

inline threadsafe_queue_impl::~threadsafe_queue_impl()
{
}

BOOST_LOG_API threadsafe_queue_impl* threadsafe_queue_impl::create(node_base* first_node)
{
    return new threadsafe_queue_impl_generic(first_node);
}

BOOST_LOG_API void threadsafe_queue_impl::destroy(threadsafe_queue_impl* impl) BOOST_NOEXCEPT
{
    delete static_cast< threadsafe_queue_impl_generic* >(impl);
}

BOOST_LOG_API threadsafe_queue_impl::node_base* threadsafe_queue_impl::reset_last_node(threadsafe_queue_impl* impl) BOOST_NOEXCEPT
{
    return static_cast< threadsafe_queue_impl_generic* >(impl)->reset_last_node();
}

BOOST_LOG_API bool threadsafe_queue_impl::unsafe_empty(const threadsafe_queue_impl* impl) BOOST_NOEXCEPT
{
    return static_cast< const threadsafe_queue_impl_generic* >(impl)->unsafe_empty();
}

BOOST_LOG_API void threadsafe_queue_impl::push(threadsafe_queue_impl* impl, node_base* p)
{
    static_cast< threadsafe_queue_impl_generic* >(impl)->push(p);
}

BOOST_LOG_API bool threadsafe_queue_impl::try_pop(threadsafe_queue_impl* impl, node_base*& node_to_free, node_base*& node_with_value)
{
    return static_cast< threadsafe_queue_impl_generic* >(impl)->try_pop(node_to_free, node_with_value);
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_NO_THREADS
