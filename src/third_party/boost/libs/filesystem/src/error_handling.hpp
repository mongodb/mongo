//  error_handling.hpp  --------------------------------------------------------------------//

//  Copyright 2002-2009, 2014 Beman Dawes
//  Copyright 2019 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM_SRC_ERROR_HANDLING_HPP_
#define BOOST_FILESYSTEM_SRC_ERROR_HANDLING_HPP_

#include <cerrno>
#include <boost/system/error_code.hpp>
#include <boost/filesystem/config.hpp>
#include <boost/filesystem/exception.hpp>

#if defined(BOOST_WINDOWS_API)
#include <boost/winapi/basic_types.hpp>
#include <boost/winapi/get_last_error.hpp>
#include <boost/winapi/error_codes.hpp>
#endif

namespace boost {
namespace filesystem {

#if defined(BOOST_POSIX_API)

typedef int err_t;

//  POSIX uses a 0 return to indicate success
#define BOOST_ERRNO errno

#define BOOST_ERROR_FILE_NOT_FOUND ENOENT
#define BOOST_ERROR_ALREADY_EXISTS EEXIST
#define BOOST_ERROR_NOT_SUPPORTED ENOSYS

#else

typedef boost::winapi::DWORD_ err_t;

//  Windows uses a non-0 return to indicate success
#define BOOST_ERRNO boost::winapi::GetLastError()

#define BOOST_ERROR_FILE_NOT_FOUND boost::winapi::ERROR_FILE_NOT_FOUND_
#define BOOST_ERROR_ALREADY_EXISTS boost::winapi::ERROR_ALREADY_EXISTS_
#define BOOST_ERROR_NOT_SUPPORTED boost::winapi::ERROR_NOT_SUPPORTED_

// Note: Legacy MinGW doesn't have ntstatus.h and doesn't define NTSTATUS error codes other than STATUS_SUCCESS.
#if !defined(NT_SUCCESS)
#define NT_SUCCESS(Status) (((boost::winapi::NTSTATUS_)(Status)) >= 0)
#endif
#if !defined(STATUS_SUCCESS)
#define STATUS_SUCCESS ((boost::winapi::NTSTATUS_)0x00000000l)
#endif
#if !defined(STATUS_NOT_IMPLEMENTED)
#define STATUS_NOT_IMPLEMENTED ((boost::winapi::NTSTATUS_)0xC0000002l)
#endif
#if !defined(STATUS_INVALID_INFO_CLASS)
#define STATUS_INVALID_INFO_CLASS ((boost::winapi::NTSTATUS_)0xC0000003l)
#endif
#if !defined(STATUS_INVALID_HANDLE)
#define STATUS_INVALID_HANDLE ((boost::winapi::NTSTATUS_)0xC0000008l)
#endif
#if !defined(STATUS_INVALID_PARAMETER)
#define STATUS_INVALID_PARAMETER ((boost::winapi::NTSTATUS_)0xC000000Dl)
#endif
#if !defined(STATUS_NO_SUCH_DEVICE)
#define STATUS_NO_SUCH_DEVICE ((boost::winapi::NTSTATUS_)0xC000000El)
#endif
#if !defined(STATUS_NO_SUCH_FILE)
#define STATUS_NO_SUCH_FILE ((boost::winapi::NTSTATUS_)0xC000000Fl)
#endif
#if !defined(STATUS_NO_MORE_FILES)
#define STATUS_NO_MORE_FILES ((boost::winapi::NTSTATUS_)0x80000006l)
#endif
#if !defined(STATUS_BUFFER_OVERFLOW)
#define STATUS_BUFFER_OVERFLOW ((boost::winapi::NTSTATUS_)0x80000005l)
#endif
#if !defined(STATUS_NO_MEMORY)
#define STATUS_NO_MEMORY ((boost::winapi::NTSTATUS_)0xC0000017l)
#endif
#if !defined(STATUS_ACCESS_DENIED)
#define STATUS_ACCESS_DENIED ((boost::winapi::NTSTATUS_)0xC0000022l)
#endif

//! Converts NTSTATUS error codes to Win32 error codes for reporting
inline boost::winapi::DWORD_ translate_ntstatus(boost::winapi::NTSTATUS_ status)
{
    // We have to cast to unsigned integral type to avoid signed overflow and narrowing conversion in the constants.
    switch (static_cast< boost::winapi::ULONG_ >(status))
    {
    case static_cast< boost::winapi::ULONG_ >(STATUS_NO_MEMORY):
        return boost::winapi::ERROR_OUTOFMEMORY_;
    case static_cast< boost::winapi::ULONG_ >(STATUS_BUFFER_OVERFLOW):
        return boost::winapi::ERROR_BUFFER_OVERFLOW_;
    case static_cast< boost::winapi::ULONG_ >(STATUS_INVALID_HANDLE):
        return boost::winapi::ERROR_INVALID_HANDLE_;
    case static_cast< boost::winapi::ULONG_ >(STATUS_INVALID_PARAMETER):
        return boost::winapi::ERROR_INVALID_PARAMETER_;
    case static_cast< boost::winapi::ULONG_ >(STATUS_NO_MORE_FILES):
        return boost::winapi::ERROR_NO_MORE_FILES_;
    case static_cast< boost::winapi::ULONG_ >(STATUS_NO_SUCH_DEVICE):
        return boost::winapi::ERROR_DEV_NOT_EXIST_;
    case static_cast< boost::winapi::ULONG_ >(STATUS_NO_SUCH_FILE):
        return boost::winapi::ERROR_FILE_NOT_FOUND_;
    case static_cast< boost::winapi::ULONG_ >(STATUS_ACCESS_DENIED):
        return boost::winapi::ERROR_ACCESS_DENIED_;
    // map "invalid info class" to "not supported" as this error likely indicates that the kernel does not support what we request
    case static_cast< boost::winapi::ULONG_ >(STATUS_INVALID_INFO_CLASS):
    default:
        return boost::winapi::ERROR_NOT_SUPPORTED_;
    }
}

#endif

//  error handling helpers  ----------------------------------------------------------//

// Implemented in exception.cpp
void emit_error(err_t error_num, system::error_code* ec, const char* message);
void emit_error(err_t error_num, path const& p, system::error_code* ec, const char* message);
void emit_error(err_t error_num, path const& p1, path const& p2, system::error_code* ec, const char* message);

inline bool error(err_t error_num, system::error_code* ec, const char* message)
{
    if (BOOST_LIKELY(!error_num))
    {
        if (ec)
            ec->clear();
        return false;
    }
    else
    { //  error
        filesystem::emit_error(error_num, ec, message);
        return true;
    }
}

inline bool error(err_t error_num, path const& p, system::error_code* ec, const char* message)
{
    if (BOOST_LIKELY(!error_num))
    {
        if (ec)
            ec->clear();
        return false;
    }
    else
    { //  error
        filesystem::emit_error(error_num, p, ec, message);
        return true;
    }
}

inline bool error(err_t error_num, path const& p1, path const& p2, system::error_code* ec, const char* message)
{
    if (BOOST_LIKELY(!error_num))
    {
        if (ec)
            ec->clear();
        return false;
    }
    else
    { //  error
        filesystem::emit_error(error_num, p1, p2, ec, message);
        return true;
    }
}

} // namespace filesystem
} // namespace boost

#endif // BOOST_FILESYSTEM_SRC_ERROR_HANDLING_HPP_
