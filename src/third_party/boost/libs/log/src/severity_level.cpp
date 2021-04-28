/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   severity_level.cpp
 * \author Andrey Semashev
 * \date   10.05.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <boost/cstdint.hpp>
#include <boost/log/sources/severity_feature.hpp>

#if !defined(BOOST_LOG_NO_THREADS) && !defined(BOOST_LOG_USE_COMPILER_TLS)
#include <boost/bind/bind.hpp>
#include <boost/checked_delete.hpp>
#include <boost/thread/thread.hpp> // at_thread_exit
#include <boost/log/detail/singleton.hpp>
#include <boost/log/detail/thread_specific.hpp>
#endif
#include "unique_ptr.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace sources {

namespace aux {

#if defined(BOOST_LOG_NO_THREADS)

static uintmax_t g_Severity = 0;

#elif defined(BOOST_LOG_USE_COMPILER_TLS)

static BOOST_LOG_TLS uintmax_t g_Severity = 0;

#else

//! Severity level storage class
class severity_level_holder :
    public boost::log::aux::lazy_singleton< severity_level_holder, boost::log::aux::thread_specific< uintmax_t* > >
{
};

#endif


#if !defined(BOOST_LOG_NO_THREADS) && !defined(BOOST_LOG_USE_COMPILER_TLS)

//! The method returns the severity level for the current thread
BOOST_LOG_API uintmax_t& get_severity_level()
{
    boost::log::aux::thread_specific< uintmax_t* >& tss = severity_level_holder::get();
    uintmax_t* p = tss.get();
    if (BOOST_UNLIKELY(!p))
    {
        log::aux::unique_ptr< uintmax_t > ptr(new uintmax_t(0));
        tss.set(ptr.get());
        p = ptr.release();
        boost::this_thread::at_thread_exit(boost::bind(checked_deleter< uintmax_t >(), p));
    }
    return *p;
}

#else // !defined(BOOST_LOG_NO_THREADS) && !defined(BOOST_LOG_USE_COMPILER_TLS)

//! The method returns the severity level for the current thread
BOOST_LOG_API uintmax_t& get_severity_level()
{
    return g_Severity;
}

#endif // !defined(BOOST_LOG_NO_THREADS) && !defined(BOOST_LOG_USE_COMPILER_TLS)

} // namespace aux

} // namespace sources

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
