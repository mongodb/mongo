/*
 *              Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   windows/mapped_shared_memory.hpp
 * \author Andrey Semashev
 * \date   13.02.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_WINDOWS_MAPPED_SHARED_MEMORY_HPP_INCLUDED_
#define BOOST_LOG_WINDOWS_MAPPED_SHARED_MEMORY_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <boost/winapi/basic_types.hpp>
#include <cstddef>
#include <boost/assert.hpp>
#include <boost/atomic/atomic.hpp>
#include <boost/log/utility/permissions.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace ipc {

namespace aux {

/*!
 * A replacement for to \c mapped_shared_memory and \c mapped_region from Boost.Interprocess.
 * The significant difference is that the shared memory name is passed as a UTF-16 string and
 * errors are reported as Boost.Log exceptions.
 */
class mapped_shared_memory
{
private:
    struct section_basic_information
    {
        void* base_address;
        boost::winapi::ULONG_ section_attributes;
        boost::winapi::LARGE_INTEGER_ section_size;
    };
    typedef boost::winapi::NTSTATUS_ (__stdcall *nt_query_section_t)(boost::winapi::HANDLE_ h, unsigned int info_class, section_basic_information* pinfo, boost::winapi::ULONG_ info_size, boost::winapi::ULONG_* ret_len);

private:
    boost::winapi::HANDLE_ m_handle;
    void* m_mapped_address;
    std::size_t m_size;
    static boost::atomic< nt_query_section_t > nt_query_section;

public:
    BOOST_CONSTEXPR mapped_shared_memory() BOOST_NOEXCEPT :
        m_handle(NULL),
        m_mapped_address(NULL),
        m_size(0u)
    {
    }

    ~mapped_shared_memory();

    //! Creates a new file mapping for the shared memory segment
    void create(const wchar_t* name, std::size_t size, permissions const& perms = permissions());
    //! Creates a new file mapping for the shared memory segment or opens the existing one. Returns \c true if the region was created and \c false if an existing one was opened.
    bool create_or_open(const wchar_t* name, std::size_t size, permissions const& perms = permissions());
    //! Opens the existing file mapping for the shared memory segment
    void open(const wchar_t* name);

    //! Maps the file mapping into the current process memory
    void map();
    //! Unmaps the file mapping
    void unmap();

    //! Returns the size of the opened shared memory segment
    std::size_t size() const BOOST_NOEXCEPT { return m_size; }
    //! Returns the address of the mapped shared memory
    void* address() const BOOST_NOEXCEPT { return m_mapped_address; }

    BOOST_DELETED_FUNCTION(mapped_shared_memory(mapped_shared_memory const&))
    BOOST_DELETED_FUNCTION(mapped_shared_memory& operator=(mapped_shared_memory const&))

private:
    //! Returns the size of the file mapping identified by the handle
    static std::size_t obtain_size(boost::winapi::HANDLE_ h);
};

} // namespace aux

} // namespace ipc

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_WINDOWS_MAPPED_SHARED_MEMORY_HPP_INCLUDED_
