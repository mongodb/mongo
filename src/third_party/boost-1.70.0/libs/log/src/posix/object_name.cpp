/*
 *             Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   posix/object_name.cpp
 * \author Andrey Semashev
 * \date   06.03.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <unistd.h>
#include <sys/types.h>
#if defined(__ANDROID__) && (__ANDROID_API__+0) < 21
#include <sys/syscall.h>
#endif
#if !defined(BOOST_LOG_NO_GETPWUID_R)
#include <pwd.h>
#endif
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <boost/move/utility_core.hpp>
#include <boost/type_traits/make_unsigned.hpp>
#include <boost/spirit/include/karma_uint.hpp>
#include <boost/spirit/include/karma_generate.hpp>
#include <boost/log/utility/ipc/object_name.hpp>
#include <boost/log/detail/header.hpp>

namespace karma = boost::spirit::karma;

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace ipc {

BOOST_LOG_ANONYMOUS_NAMESPACE {

#if defined(__ANDROID__) && (__ANDROID_API__+0) < 21
// Until Android API version 21 NDK does not define getsid wrapper in libc, although there is the corresponding syscall
inline pid_t getsid(pid_t pid) BOOST_NOEXCEPT
{
    return static_cast< pid_t >(::syscall(__NR_getsid, pid));
}
#endif

//! Formats an integer identifier into the string
template< typename Identifier >
inline void format_id(Identifier id, std::string& str)
{
    // Note: in the code below, avoid involving locale for string formatting to make sure the names are as stable as possible
    typedef typename boost::make_unsigned< Identifier >::type unsigned_id_t;
    char buf[std::numeric_limits< unsigned_id_t >::digits10 + 2];
    char* p = buf;

    typedef karma::uint_generator< unsigned_id_t, 10 > unsigned_id_gen;
    karma::generate(p, unsigned_id_gen(), static_cast< unsigned_id_t >(id));
    str.append(buf, p);
}

//! Returns a prefix string for a shared resource according to the scope
std::string get_scope_prefix(object_name::scope ns)
{
    std::string prefix = "/boost.log.";
    switch (ns)
    {
    case object_name::process_group:
        {
            prefix.append("pgid.");
#if !defined(BOOST_LOG_NO_GETPGRP)
            format_id(getpgrp(), prefix);
#else
            format_id(getuid(), prefix);
#endif
        }
        break;

    case object_name::session:
        {
            prefix.append("sid.");
#if !defined(BOOST_LOG_NO_GETSID)
            format_id(getsid(0), prefix);
#else
            format_id(getuid(), prefix);
#endif
        }
        break;

    case object_name::user:
        {
            const uid_t uid = getuid();

#if !defined(BOOST_LOG_NO_GETPWUID_R)
            long limit = sysconf(_SC_GETPW_R_SIZE_MAX);
            if (limit <= 0)
                limit = 65536;
            std::vector< char > string_storage;
            string_storage.resize(static_cast< std::size_t >(limit));
            passwd pwd = {}, *result = NULL;

            try
            {
                const int err = getpwuid_r(uid, &pwd, &string_storage[0], string_storage.size(), &result);
                if (err == 0 && result && result->pw_name)
                {
                    prefix += "user.";
                    prefix += result->pw_name;
                }
                else
                {
                    prefix += "uid.";
                    format_id(uid, prefix);
                }

                // Avoid leaving sensitive data in memory, if there is any
                std::memset(&pwd, 0, sizeof(pwd));
                std::memset(&string_storage[0], 0, string_storage.size());
            }
            catch (...)
            {
                std::memset(&pwd, 0, sizeof(pwd));
                std::memset(&string_storage[0], 0, string_storage.size());
                throw;
            }
#else
            prefix += "uid.";
            format_id(uid, prefix);
#endif
        }
        break;

    default:
        prefix += "global";
        break;
    }

    prefix.push_back('.');

    return BOOST_LOG_NRVO_RESULT(prefix);
}

} // namespace

//! Constructor from the object name
BOOST_LOG_API object_name::object_name(scope ns, const char* str) :
    m_name(get_scope_prefix(ns) + str)
{
}

//! Constructor from the object name
BOOST_LOG_API object_name::object_name(scope ns, std::string const& str) :
    m_name(get_scope_prefix(ns) + str)
{
}

} // namespace ipc

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
