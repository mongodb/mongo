/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   default_sink.cpp
 * \author Andrey Semashev
 * \date   19.04.2007
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <cstdio>
#include <boost/optional/optional.hpp>
#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/thread/locks.hpp>
#include <boost/log/detail/thread_id.hpp>
#endif
#include <boost/log/detail/default_attribute_names.hpp>
#include <boost/date_time/microsec_time_clock.hpp>
#include <boost/date_time/time_resolution_traits.hpp>
#include <boost/date_time/gregorian/gregorian_types.hpp>
#include "default_sink.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

#if !defined(BOOST_LOG_NO_THREADS)

namespace aux {

// Defined in thread_id.cpp
void format_thread_id(char* buf, std::size_t size, thread::id tid);

} // namespace aux

#endif // !defined(BOOST_LOG_NO_THREADS)

namespace sinks {

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! A special time point type that contains decomposed date and time to avoid excessive calculations
struct decomposed_time_point
{
    struct date_type :
        public gregorian::greg_year_month_day
    {
        date_type(year_type y, month_type m, day_type d) :
            gregorian::greg_year_month_day(y, m, d)
        {
        }
    };

    struct time_duration_type :
        public date_time::micro_res
    {
        typedef date_time::micro_res rep_type;

        hour_type hours;
        min_type minutes;
        sec_type seconds;
        fractional_seconds_type useconds;

        time_duration_type(hour_type h, min_type m, sec_type s, fractional_seconds_type u) :
            hours(h),
            minutes(m),
            seconds(s),
            useconds(u)
        {
        }
    };

    date_type date;
    time_duration_type time;

    decomposed_time_point(date_type const& d, time_duration_type const& t) :
        date(d),
        time(t)
    {
    }
};

inline const char* severity_level_to_string(boost::log::trivial::severity_level lvl)
{
    switch (lvl)
    {
    case boost::log::trivial::trace:
        return "[trace]  ";
    case boost::log::trivial::debug:
        return "[debug]  ";
    case boost::log::trivial::info:
        return "[info]   ";
    case boost::log::trivial::warning:
        return "[warning]";
    case boost::log::trivial::error:
        return "[error]  ";
    case boost::log::trivial::fatal:
        return "[fatal]  ";
    default:
        return "[-]      ";
    }
}

struct message_printer
{
    typedef void result_type;

    explicit message_printer(boost::log::trivial::severity_level lvl) : m_level(lvl)
    {
    }

#ifdef BOOST_LOG_USE_CHAR

    result_type operator() (std::string const& msg) const
    {
#if !defined(BOOST_LOG_NO_THREADS)
        char thread_id_buf[64];
        boost::log::aux::format_thread_id(thread_id_buf, sizeof(thread_id_buf), boost::log::aux::this_thread::get_id());
#endif

        const decomposed_time_point now = date_time::microsec_clock< decomposed_time_point >::local_time();

        std::printf("[%04u-%02u-%02u %02u:%02u:%02u.%06u] "
#if !defined(BOOST_LOG_NO_THREADS)
                    "[%s] "
#endif
                    "%s %s\n",
            static_cast< unsigned int >(now.date.year),
            static_cast< unsigned int >(now.date.month),
            static_cast< unsigned int >(now.date.day),
            static_cast< unsigned int >(now.time.hours),
            static_cast< unsigned int >(now.time.minutes),
            static_cast< unsigned int >(now.time.seconds),
            static_cast< unsigned int >(now.time.useconds),
#if !defined(BOOST_LOG_NO_THREADS)
            thread_id_buf,
#endif
            severity_level_to_string(m_level),
            msg.c_str());
    }

#endif

#ifdef BOOST_LOG_USE_WCHAR_T

    result_type operator() (std::wstring const& msg) const
    {
#if !defined(BOOST_LOG_NO_THREADS)
        char thread_id_buf[64];
        boost::log::aux::format_thread_id(thread_id_buf, sizeof(thread_id_buf), boost::log::aux::this_thread::get_id());
#endif

        const decomposed_time_point now = date_time::microsec_clock< decomposed_time_point >::local_time();

        std::printf("[%04u-%02u-%02u %02u:%02u:%02u.%06u] "
#if !defined(BOOST_LOG_NO_THREADS)
                    "[%s] "
#endif
                    "%s %ls\n",
            static_cast< unsigned int >(now.date.year),
            static_cast< unsigned int >(now.date.month),
            static_cast< unsigned int >(now.date.day),
            static_cast< unsigned int >(now.time.hours),
            static_cast< unsigned int >(now.time.minutes),
            static_cast< unsigned int >(now.time.seconds),
            static_cast< unsigned int >(now.time.useconds),
#if !defined(BOOST_LOG_NO_THREADS)
            thread_id_buf,
#endif
            severity_level_to_string(m_level),
            msg.c_str());
    }

#endif

private:
    const boost::log::trivial::severity_level m_level;
};

} // namespace

default_sink::default_sink() :
    sink(false),
    m_severity_name(boost::log::aux::default_attribute_names::severity()),
    m_message_name(boost::log::aux::default_attribute_names::message()),
    m_severity_extractor(boost::log::trivial::info)
{
}

default_sink::~default_sink()
{
}

bool default_sink::will_consume(attribute_value_set const&)
{
    return true;
}

void default_sink::consume(record_view const& rec)
{
    BOOST_LOG_EXPR_IF_MT(lock_guard< mutex_type > lock(m_mutex);)
    m_message_visitor(m_message_name, rec.attribute_values(), message_printer(m_severity_extractor(m_severity_name, rec).get()));
}

void default_sink::flush()
{
    BOOST_LOG_EXPR_IF_MT(lock_guard< mutex_type > lock(m_mutex);)
    std::fflush(stdout);
}

} // namespace aux

} // namespace sinks

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
