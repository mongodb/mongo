// Windows implementation of system_error_category
//
// Copyright Beman Dawes 2002, 2006
// Copyright (c) Microsoft Corporation 2014
// Copyright 2018 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See library home page at http://www.boost.org/libs/system

#include <boost/winapi/error_codes.hpp>
#include <boost/winapi/error_handling.hpp>
#include <boost/winapi/character_code_conversion.hpp>
#include <boost/winapi/local_memory.hpp>
#include <cstdio>

//

namespace boost
{

namespace system
{

namespace detail
{

#if ( defined(_MSC_VER) && _MSC_VER < 1900 ) || ( defined(__MINGW32__) && !defined(__MINGW64_VERSION_MAJOR) )

inline char const * unknown_message_win32( int ev, char * buffer, std::size_t len )
{
# if defined( BOOST_MSVC )
#  pragma warning( push )
#  pragma warning( disable: 4996 )
# endif

    _snprintf( buffer, len - 1, "Unknown error (%d)", ev );

    buffer[ len - 1 ] = 0;
    return buffer;

# if defined( BOOST_MSVC )
#  pragma warning( pop )
# endif
}

#else

inline char const * unknown_message_win32( int ev, char * buffer, std::size_t len )
{
    std::snprintf( buffer, len, "Unknown error (%d)", ev );
    return buffer;
}

#endif

inline boost::winapi::UINT_ message_cp_win32()
{
#if defined(BOOST_SYSTEM_USE_UTF8)

    return boost::winapi::CP_UTF8_;

#else

    return boost::winapi::CP_ACP_;

#endif
}

inline char const * system_category_message_win32( int ev, char * buffer, std::size_t len ) BOOST_NOEXCEPT
{
    if( len == 0 )
    {
        return buffer;
    }

    if( len == 1 )
    {
        buffer[0] = 0;
        return buffer;
    }

#if defined(__GNUC__)
# define BOOST_SYSTEM_ALLOCA __builtin_alloca
#else
# define BOOST_SYSTEM_ALLOCA _alloca
#endif

    wchar_t * wbuffer = static_cast<wchar_t*>( BOOST_SYSTEM_ALLOCA( len * sizeof( wchar_t ) ) );

#undef BOOST_SYSTEM_ALLOCA

    using namespace boost::winapi;

    DWORD_ retval = boost::winapi::FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM_ | FORMAT_MESSAGE_IGNORE_INSERTS_,
        NULL,
        ev,
        MAKELANGID_( LANG_NEUTRAL_, SUBLANG_DEFAULT_ ), // Default language
        wbuffer,
        static_cast<DWORD_>( len ),
        NULL
    );

    if( retval == 0 )
    {
        return unknown_message_win32( ev, buffer, len );
    }

    UINT_ const code_page = message_cp_win32();

    int r = boost::winapi::WideCharToMultiByte( code_page, 0, wbuffer, -1, buffer, static_cast<int>( len ), NULL, NULL );

    if( r == 0 )
    {
        return unknown_message_win32( ev, buffer, len );
    }

    --r; // exclude null terminator

    while( r > 0 && ( buffer[ r-1 ] == '\n' || buffer[ r-1 ] == '\r' ) )
    {
        buffer[ --r ] = 0;
    }

    if( r > 0 && buffer[ r-1 ] == '.' )
    {
        buffer[ --r ] = 0;
    }

    return buffer;
}

struct local_free
{
    void * p_;

    ~local_free()
    {
        boost::winapi::LocalFree( p_ );
    }
};

inline std::string unknown_message_win32( int ev )
{
    char buffer[ 38 ];
    return unknown_message_win32( ev, buffer, sizeof( buffer ) );
}

inline std::string system_category_message_win32( int ev )
{
    using namespace boost::winapi;

    wchar_t * lpMsgBuf = 0;

    DWORD_ retval = boost::winapi::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER_ | FORMAT_MESSAGE_FROM_SYSTEM_ | FORMAT_MESSAGE_IGNORE_INSERTS_,
        NULL,
        ev,
        MAKELANGID_( LANG_NEUTRAL_, SUBLANG_DEFAULT_ ), // Default language
        (LPWSTR_) &lpMsgBuf,
        0,
        NULL
    );

    if( retval == 0 )
    {
        return unknown_message_win32( ev );
    }

    local_free lf_ = { lpMsgBuf };
    (void)lf_;

    UINT_ const code_page = message_cp_win32();

    int r = boost::winapi::WideCharToMultiByte( code_page, 0, lpMsgBuf, -1, 0, 0, NULL, NULL );

    if( r == 0 )
    {
        return unknown_message_win32( ev );
    }

    std::string buffer( r, char() );

    r = boost::winapi::WideCharToMultiByte( code_page, 0, lpMsgBuf, -1, &buffer[0], r, NULL, NULL );

    if( r == 0 )
    {
        return unknown_message_win32( ev );
    }

    --r; // exclude null terminator

    while( r > 0 && ( buffer[ r-1 ] == '\n' || buffer[ r-1 ] == '\r' ) )
    {
        --r;
    }

    if( r > 0 && buffer[ r-1 ] == '.' )
    {
        --r;
    }

    buffer.resize( r );

    return buffer;
}

inline error_condition system_category_default_error_condition_win32( int ev ) BOOST_NOEXCEPT
{
    // When using the Windows Runtime, most system errors are reported as HRESULTs.
    // We want to map the common Win32 errors to their equivalent error condition,
    // whether or not they are reported via an HRESULT.

#define BOOST_SYSTEM_FAILED(hr)           ((hr) < 0)
#define BOOST_SYSTEM_HRESULT_FACILITY(hr) (((hr) >> 16) & 0x1fff)
#define BOOST_SYSTEM_HRESULT_CODE(hr)     ((hr) & 0xFFFF)
#define BOOST_SYSTEM_FACILITY_WIN32       7

    if( BOOST_SYSTEM_FAILED( ev ) && BOOST_SYSTEM_HRESULT_FACILITY( ev ) == BOOST_SYSTEM_FACILITY_WIN32 )
    {
        ev = BOOST_SYSTEM_HRESULT_CODE( ev );
    }

#undef BOOST_SYSTEM_FAILED
#undef BOOST_SYSTEM_HRESULT_FACILITY
#undef BOOST_SYSTEM_HRESULT_CODE
#undef BOOST_SYSTEM_FACILITY_WIN32

    using namespace boost::winapi;
    using namespace errc;

    // Windows system -> posix_errno decode table
    // see WinError.h comments for descriptions of errors

    switch ( ev )
    {
    case 0: return make_error_condition( success );

    case ERROR_ACCESS_DENIED_: return make_error_condition( permission_denied );
    case ERROR_ALREADY_EXISTS_: return make_error_condition( file_exists );
    case ERROR_BAD_UNIT_: return make_error_condition( no_such_device );
    case ERROR_BUFFER_OVERFLOW_: return make_error_condition( filename_too_long );
    case ERROR_BUSY_: return make_error_condition( device_or_resource_busy );
    case ERROR_BUSY_DRIVE_: return make_error_condition( device_or_resource_busy );
    case ERROR_CANNOT_MAKE_: return make_error_condition( permission_denied );
    case ERROR_CANTOPEN_: return make_error_condition( io_error );
    case ERROR_CANTREAD_: return make_error_condition( io_error );
    case ERROR_CANTWRITE_: return make_error_condition( io_error );
    case ERROR_CURRENT_DIRECTORY_: return make_error_condition( permission_denied );
    case ERROR_DEV_NOT_EXIST_: return make_error_condition( no_such_device );
    case ERROR_DEVICE_IN_USE_: return make_error_condition( device_or_resource_busy );
    case ERROR_DIR_NOT_EMPTY_: return make_error_condition( directory_not_empty );
    case ERROR_DIRECTORY_: return make_error_condition( invalid_argument ); // WinError.h: "The directory name is invalid"
    case ERROR_DISK_FULL_: return make_error_condition( no_space_on_device );
    case ERROR_FILE_EXISTS_: return make_error_condition( file_exists );
    case ERROR_FILE_NOT_FOUND_: return make_error_condition( no_such_file_or_directory );
    case ERROR_HANDLE_DISK_FULL_: return make_error_condition( no_space_on_device );
    case ERROR_INVALID_ACCESS_: return make_error_condition( permission_denied );
    case ERROR_INVALID_DRIVE_: return make_error_condition( no_such_device );
    case ERROR_INVALID_FUNCTION_: return make_error_condition( function_not_supported );
    case ERROR_INVALID_HANDLE_: return make_error_condition( invalid_argument );
    case ERROR_INVALID_NAME_: return make_error_condition( invalid_argument );
    case ERROR_LOCK_VIOLATION_: return make_error_condition( no_lock_available );
    case ERROR_LOCKED_: return make_error_condition( no_lock_available );
    case ERROR_NEGATIVE_SEEK_: return make_error_condition( invalid_argument );
    case ERROR_NOACCESS_: return make_error_condition( permission_denied );
    case ERROR_NOT_ENOUGH_MEMORY_: return make_error_condition( not_enough_memory );
    case ERROR_NOT_READY_: return make_error_condition( resource_unavailable_try_again );
    case ERROR_NOT_SAME_DEVICE_: return make_error_condition( cross_device_link );
    case ERROR_OPEN_FAILED_: return make_error_condition( io_error );
    case ERROR_OPEN_FILES_: return make_error_condition( device_or_resource_busy );
    case ERROR_OPERATION_ABORTED_: return make_error_condition( operation_canceled );
    case ERROR_OUTOFMEMORY_: return make_error_condition( not_enough_memory );
    case ERROR_PATH_NOT_FOUND_: return make_error_condition( no_such_file_or_directory );
    case ERROR_READ_FAULT_: return make_error_condition( io_error );
    case ERROR_RETRY_: return make_error_condition( resource_unavailable_try_again );
    case ERROR_SEEK_: return make_error_condition( io_error );
    case ERROR_SHARING_VIOLATION_: return make_error_condition( permission_denied );
    case ERROR_TOO_MANY_OPEN_FILES_: return make_error_condition( too_many_files_open );
    case ERROR_WRITE_FAULT_: return make_error_condition( io_error );
    case ERROR_WRITE_PROTECT_: return make_error_condition( permission_denied );
    case WSAEACCES_: return make_error_condition( permission_denied );
    case WSAEADDRINUSE_: return make_error_condition( address_in_use );
    case WSAEADDRNOTAVAIL_: return make_error_condition( address_not_available );
    case WSAEAFNOSUPPORT_: return make_error_condition( address_family_not_supported );
    case WSAEALREADY_: return make_error_condition( connection_already_in_progress );
    case WSAEBADF_: return make_error_condition( bad_file_descriptor );
    case WSAECONNABORTED_: return make_error_condition( connection_aborted );
    case WSAECONNREFUSED_: return make_error_condition( connection_refused );
    case WSAECONNRESET_: return make_error_condition( connection_reset );
    case WSAEDESTADDRREQ_: return make_error_condition( destination_address_required );
    case WSAEFAULT_: return make_error_condition( bad_address );
    case WSAEHOSTUNREACH_: return make_error_condition( host_unreachable );
    case WSAEINPROGRESS_: return make_error_condition( operation_in_progress );
    case WSAEINTR_: return make_error_condition( interrupted );
    case WSAEINVAL_: return make_error_condition( invalid_argument );
    case WSAEISCONN_: return make_error_condition( already_connected );
    case WSAEMFILE_: return make_error_condition( too_many_files_open );
    case WSAEMSGSIZE_: return make_error_condition( message_size );
    case WSAENAMETOOLONG_: return make_error_condition( filename_too_long );
    case WSAENETDOWN_: return make_error_condition( network_down );
    case WSAENETRESET_: return make_error_condition( network_reset );
    case WSAENETUNREACH_: return make_error_condition( network_unreachable );
    case WSAENOBUFS_: return make_error_condition( no_buffer_space );
    case WSAENOPROTOOPT_: return make_error_condition( no_protocol_option );
    case WSAENOTCONN_: return make_error_condition( not_connected );
    case WSAENOTSOCK_: return make_error_condition( not_a_socket );
    case WSAEOPNOTSUPP_: return make_error_condition( operation_not_supported );
    case WSAEPROTONOSUPPORT_: return make_error_condition( protocol_not_supported );
    case WSAEPROTOTYPE_: return make_error_condition( wrong_protocol_type );
    case WSAETIMEDOUT_: return make_error_condition( timed_out );
    case WSAEWOULDBLOCK_: return make_error_condition( operation_would_block );

    default: return error_condition( ev, system_category() );
    }
}

} // namespace detail

} // namespace system

} // namespace boost
