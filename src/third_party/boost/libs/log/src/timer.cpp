/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   timer.cpp
 * \author Andrey Semashev
 * \date   02.12.2007
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <boost/log/attributes/timer.hpp>
#include <boost/log/attributes/attribute_value_impl.hpp>

#if defined(BOOST_WINDOWS) && !defined(BOOST_LOG_NO_QUERY_PERFORMANCE_COUNTER)

#include <boost/assert.hpp>
#include <boost/cstdint.hpp>
#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/log/detail/locks.hpp>
#include <boost/thread/mutex.hpp>
#endif
#include <windows.h>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace attributes {

//! Factory implementation
class BOOST_SYMBOL_VISIBLE timer::impl :
    public attribute::impl
{
private:
#if !defined(BOOST_LOG_NO_THREADS)
    //! Synchronization mutex type
    typedef boost::mutex mutex_type;
    //! Synchronization mutex
    mutex_type m_Mutex;
#endif
    //! Frequency factor for calculating duration
    double m_FrequencyFactor;
    //! Last value of the performance counter
    uint64_t m_LastCounter;
    //! Elapsed time duration, in microseconds
    uint64_t m_Duration;

public:
    //! Constructor
    impl() : m_Duration(0)
    {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        BOOST_ASSERT(li.QuadPart != 0LL);
        m_FrequencyFactor = 1000000.0 / static_cast< double >(li.QuadPart);

        QueryPerformanceCounter(&li);
        m_LastCounter = static_cast< uint64_t >(li.QuadPart);
    }

    //! The method returns the actual attribute value. It must not return NULL.
    attribute_value get_value()
    {
        uint64_t duration;
        {
            BOOST_LOG_EXPR_IF_MT(log::aux::exclusive_lock_guard< mutex_type > lock(m_Mutex);)

            LARGE_INTEGER li;
            QueryPerformanceCounter(&li);
            const uint64_t counter = static_cast< uint64_t >(li.QuadPart);
            const uint64_t counts = counter - m_LastCounter;
            m_LastCounter = counter;
            duration = m_Duration + static_cast< uint64_t >(counts * m_FrequencyFactor);
            m_Duration = duration;
        }

        return attribute_value(new attribute_value_impl< value_type >(boost::posix_time::microseconds(duration)));
    }
};

//! Constructor
timer::timer() : attribute(new impl())
{
}

//! Constructor for casting support
timer::timer(cast_source const& source) : attribute(source.as< impl >())
{
}

} // namespace attributes

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#else // defined(BOOST_WINDOWS) && !defined(BOOST_LOG_NO_QUERY_PERFORMANCE_COUNTER)

#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace attributes {

//! Factory implementation
class BOOST_SYMBOL_VISIBLE timer::impl :
    public attribute::impl
{
public:
    //! Time type
    typedef utc_time_traits::time_type time_type;

private:
    //! Base time point
    const time_type m_BaseTimePoint;

public:
    /*!
     * Constructor. Starts time counting.
     */
    impl() : m_BaseTimePoint(utc_time_traits::get_clock()) {}

    attribute_value get_value() BOOST_OVERRIDE
    {
        return attribute_value(new attribute_value_impl< value_type >(
            utc_time_traits::get_clock() - m_BaseTimePoint));
    }
};

//! Constructor
timer::timer() : attribute(new impl())
{
}

//! Constructor for casting support
timer::timer(cast_source const& source) : attribute(source.as< impl >())
{
}

} // namespace attributes

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // defined(BOOST_WINDOWS) && !defined(BOOST_LOG_NO_QUERY_PERFORMANCE_COUNTER)
