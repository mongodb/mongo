/*
 *             Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   windows/object_name.cpp
 * \author Andrey Semashev
 * \date   06.03.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <boost/memory_order.hpp>
#include <boost/atomic/atomic.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/utility/ipc/object_name.hpp>
#include <boost/winapi/get_last_error.hpp>
#include <windows.h>
#include <lmcons.h>
#include <security.h>
#include "windows/auto_handle.hpp"
#include "windows/utf_code_conversion.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace ipc {

BOOST_LOG_ANONYMOUS_NAMESPACE {

#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

class auto_boundary_descriptor
{
private:
    HANDLE m_handle;

public:
    explicit auto_boundary_descriptor(HANDLE h = NULL) BOOST_NOEXCEPT : m_handle(h)
    {
    }

    ~auto_boundary_descriptor() BOOST_NOEXCEPT
    {
        if (m_handle)
            DeleteBoundaryDescriptor(m_handle);
    }

    void init(HANDLE h) BOOST_NOEXCEPT
    {
        BOOST_ASSERT(m_handle == NULL);
        m_handle = h;
    }

    HANDLE get() const BOOST_NOEXCEPT { return m_handle; }
    HANDLE* get_ptr() BOOST_NOEXCEPT { return &m_handle; }

    void swap(auto_boundary_descriptor& that) BOOST_NOEXCEPT
    {
        HANDLE h = m_handle;
        m_handle = that.m_handle;
        that.m_handle = h;
    }

    BOOST_DELETED_FUNCTION(auto_boundary_descriptor(auto_boundary_descriptor const&))
    BOOST_DELETED_FUNCTION(auto_boundary_descriptor& operator=(auto_boundary_descriptor const&))
};

//! Handle for the private namespace for \c user scope
static boost::atomic< HANDLE > g_user_private_namespace;

//! Closes the private namespace on process exit
void close_user_namespace()
{
    HANDLE h = g_user_private_namespace.load(boost::memory_order_acquire);
    if (h)
    {
        ClosePrivateNamespace(h, 0);
        g_user_private_namespace.store((HANDLE)NULL, boost::memory_order_release);
    }
}

//! Attempts to create or open the private namespace
bool init_user_namespace()
{
    HANDLE h = g_user_private_namespace.load(boost::memory_order_acquire);
    if (BOOST_UNLIKELY(!h))
    {
        // Obtain the current user SID
        boost::log::ipc::aux::auto_handle h_process_token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, h_process_token.get_ptr()))
            return false;

        DWORD token_user_size = 0;
        GetTokenInformation(h_process_token.get(), TokenUser, NULL, 0u, &token_user_size);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || token_user_size < sizeof(TOKEN_USER))
            return false;
        std::vector< unsigned char > token_user_storage(static_cast< std::size_t >(token_user_size), static_cast< unsigned char >(0u));
        if (!GetTokenInformation(h_process_token.get(), TokenUser, &token_user_storage[0], token_user_size, &token_user_size))
            return false;

        TOKEN_USER const& token_user = *reinterpret_cast< const TOKEN_USER* >(&token_user_storage[0]);
        if (!token_user.User.Sid)
            return false;

        // Create a boundary descriptor with the user's SID
        auto_boundary_descriptor h_boundary(CreateBoundaryDescriptorW(L"User", 0));
        if (!h_boundary.get())
            return false;

        if (!AddSIDToBoundaryDescriptor(h_boundary.get_ptr(), token_user.User.Sid))
            return false;

        // Create or open a namespace for kernel objects
        h = CreatePrivateNamespaceW(NULL, h_boundary.get(), L"User");
        if (!h)
            h = OpenPrivateNamespaceW(h_boundary.get(), L"User");

        if (h)
        {
            HANDLE expected = NULL;
            if (g_user_private_namespace.compare_exchange_strong(expected, h, boost::memory_order_acq_rel, boost::memory_order_acquire))
            {
                std::atexit(&close_user_namespace);
            }
            else
            {
                // Another thread must have opened the namespace
                ClosePrivateNamespace(h, 0);
                h = expected;
            }
        }
    }

    return !!h;
}

#endif // BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6

//! Returns a prefix string for a shared resource according to the scope
std::string get_scope_prefix(object_name::scope ns)
{
    std::string prefix;
    switch (ns)
    {
    case object_name::process_group:
        {
            // For now consider all processes as members of the common process group. It may change if there is found
            // a way to get a process group id (i.e. id of the closest parent process that was created with the CREATE_NEW_PROCESS_GROUP flag).
            prefix = "Local\\boost.log.process_group";
        }
        break;

    case object_name::session:
        {
            prefix = "Local\\boost.log.session";
        }
        break;

    case object_name::user:
        {
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
            if (init_user_namespace())
            {
                prefix = "User\\boost.log.user";
            }
            else
#endif // BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
            {
                wchar_t buf[UNLEN + 1u];
                ULONG len = sizeof(buf) / sizeof(*buf);
                if (BOOST_UNLIKELY(!GetUserNameExW(NameSamCompatible, buf, &len)))
                {
                    const boost::winapi::DWORD_ err = boost::winapi::GetLastError();
                    BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to obtain the current user name", (err));
                }

                std::replace(buf, buf + len, L'\\', L'.');

                prefix = "Local\\boost.log.user." + boost::log::aux::utf16_to_utf8(buf);
            }
        }
        break;

    default:
        prefix = "Global\\boost.log.global";
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
