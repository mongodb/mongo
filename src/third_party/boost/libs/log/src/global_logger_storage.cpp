/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   global_logger_storage.cpp
 * \author Andrey Semashev
 * \date   21.04.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <map>
#include <string>
#include <boost/limits.hpp>
#include <boost/type_index.hpp>
#include <boost/core/snprintf.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/detail/singleton.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#if !defined(BOOST_LOG_NO_THREADS)
#include <mutex>
#endif
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace sources {

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! The loggers repository singleton
struct loggers_repository :
    public log::aux::lazy_singleton< loggers_repository >
{
    //! Repository map type
    typedef std::map< typeindex::type_index, shared_ptr< logger_holder_base > > loggers_map_t;

#if !defined(BOOST_LOG_NO_THREADS)
    //! Synchronization primitive
    mutable std::mutex m_Mutex;
#endif
    //! Map of logger holders
    loggers_map_t m_Loggers;
};

} // namespace

//! Finds or creates the logger and returns its holder
BOOST_LOG_API shared_ptr< logger_holder_base > global_storage::get_or_init(typeindex::type_index key, initializer_t initializer)
{
    typedef loggers_repository::loggers_map_t loggers_map_t;
    loggers_repository& repo = loggers_repository::get();

    BOOST_LOG_EXPR_IF_MT(std::lock_guard< std::mutex > lock(repo.m_Mutex);)
    loggers_map_t::iterator it = repo.m_Loggers.find(key);
    if (it != repo.m_Loggers.end())
    {
        // There is an instance
        return it->second;
    }
    else
    {
        // We have to create a logger instance
        shared_ptr< logger_holder_base > inst = initializer();
        repo.m_Loggers[key] = inst;
        return inst;
    }
}

//! Throws the \c odr_violation exception
BOOST_LOG_API BOOST_LOG_NORETURN void throw_odr_violation(
    typeindex::type_index tag_type,
    typeindex::type_index logger_type,
    logger_holder_base const& registered)
{
    std::string str("Could not initialize global logger with tag \"");
    str.append(tag_type.pretty_name());
    str.append("\" and type \"");
    str.append(logger_type.pretty_name());
    str.append("\". A logger of type \"");
    str.append(registered.m_LoggerType.pretty_name());
    str.append("\" with the same tag has already been registered at ");
    str.append(registered.m_RegistrationFile);

    char buf[std::numeric_limits< unsigned int >::digits10 + 3];
    int res = boost::core::snprintf(buf, sizeof(buf), "%u", registered.m_RegistrationLine);
    if (BOOST_LIKELY(res > 0))
    {
        if (BOOST_UNLIKELY(static_cast< unsigned int >(res) >= sizeof(buf)))
            res = sizeof(buf) - 1u;

        str.push_back(':');
        str.append(buf, res);
    }

    str.push_back('.');

    BOOST_LOG_THROW_DESCR(odr_violation, str);
}

} // namespace aux

} // namespace sources

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
