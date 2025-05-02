/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   unbounded_ordering_queue.hpp
 * \author Andrey Semashev
 * \date   24.07.2011
 *
 * The header contains implementation of unbounded ordering record queueing strategy for
 * the asynchronous sink frontend.
 */

#ifndef BOOST_LOG_SINKS_UNBOUNDED_ORDERING_QUEUE_HPP_INCLUDED_
#define BOOST_LOG_SINKS_UNBOUNDED_ORDERING_QUEUE_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if defined(BOOST_LOG_NO_THREADS)
#error Boost.Log: This header content is only supported in multithreaded environment
#endif

#include <queue>
#include <vector>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <boost/log/detail/enqueued_record.hpp>
#include <boost/log/keywords/order.hpp>
#include <boost/log/keywords/ordering_window.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace sinks {

/*!
 * \brief Unbounded ordering log record queueing strategy
 *
 * The \c unbounded_ordering_queue class is intended to be used with
 * the \c asynchronous_sink frontend as a log record queueing strategy.
 *
 * This strategy provides the following properties to the record queueing mechanism:
 *
 * \li The queue has no size limits.
 * \li The queue has a fixed latency window. This means that each log record put
 *     into the queue will normally not be dequeued for a certain period of time.
 * \li The queue performs stable record ordering within the latency window.
 *     The ordering predicate can be specified in the \c OrderT template parameter.
 *
 * Since this queue has no size limits, it may grow uncontrollably if sink backends
 * dequeue log records not fast enough. When this is an issue, it is recommended to
 * use one of the bounded strategies.
 */
template< typename OrderT >
class unbounded_ordering_queue
{
private:
    typedef std::mutex mutex_type;
    typedef sinks::aux::enqueued_record enqueued_record;

    typedef std::priority_queue<
        enqueued_record,
        std::vector< enqueued_record >,
        enqueued_record::order< OrderT >
    > queue_type;

private:
    //! Ordering window duration
    const std::chrono::steady_clock::duration m_ordering_window;
    //! Synchronization mutex
    mutex_type m_mutex;
    //! Condition for blocking
    std::condition_variable m_cond;
    //! Thread-safe queue
    queue_type m_queue;
    //! Interruption flag
    bool m_interruption_requested;

public:
    /*!
     * Returns ordering window size specified during initialization
     */
    std::chrono::steady_clock::duration get_ordering_window() const
    {
        return m_ordering_window;
    }

    /*!
     * Returns default ordering window size.
     * The default window size is specific to the operating system thread scheduling mechanism.
     */
    static BOOST_CONSTEXPR std::chrono::steady_clock::duration get_default_ordering_window() BOOST_NOEXCEPT
    {
        // The main idea behind this parameter is that the ordering window should be large enough
        // to allow the frontend to order records from different threads on an attribute
        // that contains system time. Thus this value should be:
        // * No less than the minimum time resolution quant that Boost.DateTime provides on the current OS.
        //   For instance, on Windows it defaults to around 15-16 ms.
        // * No less than thread switching quant on the current OS. For now 30 ms is large enough window size to
        //   switch threads on any known OS. It can be tuned for other platforms as needed.
        return std::chrono::milliseconds(30);
    }

protected:
    //! Initializing constructor
    template< typename ArgsT >
    explicit unbounded_ordering_queue(ArgsT const& args) :
        m_ordering_window(std::chrono::duration_cast< std::chrono::steady_clock::duration >(args[keywords::ordering_window || &unbounded_ordering_queue::get_default_ordering_window])),
        m_queue(args[keywords::order]),
        m_interruption_requested(false)
    {
    }

    //! Enqueues log record to the queue
    void enqueue(record_view const& rec)
    {
        std::lock_guard< mutex_type > lock(m_mutex);
        enqueue_unlocked(rec);
    }

    //! Attempts to enqueue log record to the queue
    bool try_enqueue(record_view const& rec)
    {
        std::unique_lock< mutex_type > lock(m_mutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            enqueue_unlocked(rec);
            return true;
        }
        else
            return false;
    }

    //! Attempts to dequeue a log record ready for processing from the queue, does not block if no log records are ready to be processed
    bool try_dequeue_ready(record_view& rec)
    {
        std::lock_guard< mutex_type > lock(m_mutex);
        if (!m_queue.empty())
        {
            const auto now = std::chrono::steady_clock::now();
            enqueued_record const& elem = m_queue.top();
            if ((now - elem.m_timestamp) >= m_ordering_window)
            {
                // We got a new element
                rec = elem.m_record;
                m_queue.pop();
                return true;
            }
        }

        return false;
    }

    //! Attempts to dequeue log record from the queue, does not block.
    bool try_dequeue(record_view& rec)
    {
        std::lock_guard< mutex_type > lock(m_mutex);
        if (!m_queue.empty())
        {
            enqueued_record const& elem = m_queue.top();
            rec = elem.m_record;
            m_queue.pop();
            return true;
        }

        return false;
    }

    //! Dequeues log record from the queue, blocks if no log records are ready to be processed
    bool dequeue_ready(record_view& rec)
    {
        std::unique_lock< mutex_type > lock(m_mutex);
        while (!m_interruption_requested)
        {
            if (!m_queue.empty())
            {
                const auto now = std::chrono::steady_clock::now();
                enqueued_record const& elem = m_queue.top();
                const auto difference = now - elem.m_timestamp;
                if (difference >= m_ordering_window)
                {
                    // We got a new element
                    rec = elem.m_record;
                    m_queue.pop();
                    return true;
                }
                else
                {
                    // Wait until the element becomes ready to be processed
                    m_cond.wait_for(lock, m_ordering_window - difference);
                }
            }
            else
            {
                // Wait for an element to come
                m_cond.wait(lock);
            }
        }
        m_interruption_requested = false;

        return false;
    }

    //! Wakes a thread possibly blocked in the \c dequeue method
    void interrupt_dequeue()
    {
        std::lock_guard< mutex_type > lock(m_mutex);
        m_interruption_requested = true;
        m_cond.notify_one();
    }

private:
    //! Enqueues a log record
    void enqueue_unlocked(record_view const& rec)
    {
        const bool was_empty = m_queue.empty();
        m_queue.push(enqueued_record(rec));
        if (was_empty)
            m_cond.notify_one();
    }
};

} // namespace sinks

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_SINKS_UNBOUNDED_ORDERING_QUEUE_HPP_INCLUDED_
