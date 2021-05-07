/*
 *             Copyright Andrey Semashev 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   permissions.cpp
 * \author Andrey Semashev
 * \date   26.12.2015
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <boost/log/utility/permissions.hpp>
#include <boost/interprocess/permissions.hpp>

#if defined(BOOST_WINDOWS)

#include <boost/log/exceptions.hpp>
#include <boost/throw_exception.hpp>
#include <boost/log/utility/once_block.hpp>
#include <windows.h>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

BOOST_LOG_ANONYMOUS_NAMESPACE {

static ::SECURITY_DESCRIPTOR g_unrestricted_security_descriptor;
static ::SECURITY_ATTRIBUTES g_unrestricted_security_attributes;

} // namespace

BOOST_LOG_API permissions::native_type permissions::get_unrestricted_security_attributes()
{
    BOOST_LOG_ONCE_BLOCK()
    {
        if (!InitializeSecurityDescriptor(&g_unrestricted_security_descriptor, SECURITY_DESCRIPTOR_REVISION))
        {
            DWORD err = GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to initialize security descriptor", (err));
        }

        if (!SetSecurityDescriptorDacl(&g_unrestricted_security_descriptor, TRUE, NULL, FALSE))
        {
            DWORD err = GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(system_error, "Failed to set null DACL to a security descriptor", (err));
        }

        g_unrestricted_security_attributes.nLength = sizeof(g_unrestricted_security_attributes);
        g_unrestricted_security_attributes.lpSecurityDescriptor = &g_unrestricted_security_descriptor;
        g_unrestricted_security_attributes.bInheritHandle = FALSE;
    }

    return &g_unrestricted_security_attributes;
}

//! Initializing constructor
BOOST_LOG_API permissions::permissions(boost::interprocess::permissions const& perms) BOOST_NOEXCEPT :
    m_perms(reinterpret_cast< native_type >(perms.get_permissions()))
{
}


BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#else // defined(BOOST_WINDOWS)

#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

//! Initializing constructor
BOOST_LOG_API permissions::permissions(boost::interprocess::permissions const& perms) BOOST_NOEXCEPT :
    m_perms(static_cast< native_type >(perms.get_permissions()))
{
}

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#endif // defined(BOOST_WINDOWS)

#include <boost/log/detail/footer.hpp>
