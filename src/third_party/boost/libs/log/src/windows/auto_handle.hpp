/*
 *              Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   windows/auto_handle.hpp
 * \author Andrey Semashev
 * \date   07.03.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_WINDOWS_AUTO_HANDLE_HPP_INCLUDED_
#define BOOST_LOG_WINDOWS_AUTO_HANDLE_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <boost/assert.hpp>
#include <boost/winapi/handles.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace ipc {

namespace aux {

//! A wrapper around a kernel object handle. Automatically closes the handle on destruction.
class auto_handle
{
private:
    boost::winapi::HANDLE_ m_handle;

public:
    explicit auto_handle(boost::winapi::HANDLE_ h = NULL) BOOST_NOEXCEPT : m_handle(h)
    {
    }

    ~auto_handle() BOOST_NOEXCEPT
    {
        if (m_handle)
            BOOST_VERIFY(boost::winapi::CloseHandle(m_handle) != 0);
    }

    void init(boost::winapi::HANDLE_ h) BOOST_NOEXCEPT
    {
        BOOST_ASSERT(m_handle == NULL);
        m_handle = h;
    }

    boost::winapi::HANDLE_ get() const BOOST_NOEXCEPT { return m_handle; }
    boost::winapi::HANDLE_* get_ptr() BOOST_NOEXCEPT { return &m_handle; }

    void swap(auto_handle& that) BOOST_NOEXCEPT
    {
        boost::winapi::HANDLE_ h = m_handle;
        m_handle = that.m_handle;
        that.m_handle = h;
    }

    BOOST_DELETED_FUNCTION(auto_handle(auto_handle const&))
    BOOST_DELETED_FUNCTION(auto_handle& operator=(auto_handle const&))
};

} // namespace aux

} // namespace ipc

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_WINDOWS_AUTO_HANDLE_HPP_INCLUDED_
