//  windows_tools.hpp  -----------------------------------------------------------------//

//  Copyright 2002-2009, 2014 Beman Dawes
//  Copyright 2001 Dietmar Kuehl

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM_SRC_WINDOWS_TOOLS_HPP_
#define BOOST_FILESYSTEM_SRC_WINDOWS_TOOLS_HPP_

#include <cstddef>
#include <boost/filesystem/config.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/winapi/basic_types.hpp> // NTSTATUS_

#include <windows.h>

#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT (0xA0000003L)
#endif

#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK (0xA000000CL)
#endif

namespace boost {
namespace filesystem {
namespace detail {

BOOST_INLINE_VARIABLE BOOST_CONSTEXPR_OR_CONST wchar_t colon = L':';
BOOST_INLINE_VARIABLE BOOST_CONSTEXPR_OR_CONST wchar_t questionmark = L'?';

inline bool is_letter(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

inline bool equal_extension(wchar_t const* p, wchar_t const (&x1)[5], wchar_t const (&x2)[5])
{
    return (p[0] == x1[0] || p[0] == x2[0]) &&
        (p[1] == x1[1] || p[1] == x2[1]) &&
        (p[2] == x1[2] || p[2] == x2[2]) &&
        (p[3] == x1[3] || p[3] == x2[3]) &&
        p[4] == 0;
}

inline boost::filesystem::perms make_permissions(boost::filesystem::path const& p, DWORD attr)
{
    boost::filesystem::perms prms = boost::filesystem::owner_read | boost::filesystem::group_read | boost::filesystem::others_read;
    if ((attr & FILE_ATTRIBUTE_READONLY) == 0u)
        prms |= boost::filesystem::owner_write | boost::filesystem::group_write | boost::filesystem::others_write;
    boost::filesystem::path ext = p.extension();
    wchar_t const* q = ext.c_str();
    if (equal_extension(q, L".exe", L".EXE") || equal_extension(q, L".com", L".COM") || equal_extension(q, L".bat", L".BAT") || equal_extension(q, L".cmd", L".CMD"))
        prms |= boost::filesystem::owner_exe | boost::filesystem::group_exe | boost::filesystem::others_exe;
    return prms;
}

bool is_reparse_point_a_symlink_ioctl(HANDLE h);

inline bool is_reparse_point_tag_a_symlink(ULONG reparse_point_tag)
{
    return reparse_point_tag == IO_REPARSE_TAG_SYMLINK
        // Issue 9016 asked that NTFS directory junctions be recognized as directories.
        // That is equivalent to recognizing them as symlinks, and then the normal symlink
        // mechanism will take care of recognizing them as directories.
        //
        // Directory junctions are very similar to symlinks, but have some performance
        // and other advantages over symlinks. They can be created from the command line
        // with "mklink /J junction-name target-path".
        //
        // Note that mounted filesystems also have the same repartse point tag, which makes
        // them look like directory symlinks in terms of Boost.Filesystem. read_symlink()
        // may return a volume path or NT path for such symlinks.
        || reparse_point_tag == IO_REPARSE_TAG_MOUNT_POINT; // aka "directory junction" or "junction"
}

#if !defined(UNDER_CE)

//! IO_STATUS_BLOCK definition from Windows SDK.
struct io_status_block
{
    union
    {
        boost::winapi::NTSTATUS_ Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

//! UNICODE_STRING definition from Windows SDK
struct unicode_string
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

//! PIO_APC_ROUTINE definition from Windows SDK
typedef VOID (NTAPI* pio_apc_routine) (PVOID ApcContext, io_status_block* IoStatusBlock, ULONG Reserved);

//! FILE_INFORMATION_CLASS enum entries
enum file_information_class
{
    file_directory_information_class = 1
};

//! NtQueryDirectoryFile signature. Available since Windows NT 4.0 (probably).
typedef boost::winapi::NTSTATUS_ (NTAPI NtQueryDirectoryFile_t)(
  /*in*/ HANDLE FileHandle,
  /*in, optional*/ HANDLE Event,
  /*in, optional*/ pio_apc_routine ApcRoutine,
  /*in, optional*/ PVOID ApcContext,
  /*out*/ io_status_block* IoStatusBlock,
  /*out*/ PVOID FileInformation,
  /*in*/ ULONG Length,
  /*in*/ file_information_class FileInformationClass,
  /*in*/ BOOLEAN ReturnSingleEntry,
  /*in, optional*/ unicode_string* FileName,
  /*in*/ BOOLEAN RestartScan);

extern NtQueryDirectoryFile_t* nt_query_directory_file_api;

#endif // !defined(UNDER_CE)

//! FILE_INFO_BY_HANDLE_CLASS enum entries
enum file_info_by_handle_class
{
    file_basic_info_class = 0,
    file_disposition_info_class = 4,
    file_attribute_tag_info_class = 9,
    file_id_both_directory_info_class = 10,
    file_id_both_directory_restart_info_class = 11,
    file_full_directory_info_class = 14,
    file_full_directory_restart_info_class = 15,
    file_id_extd_directory_info_class = 19,
    file_id_extd_directory_restart_info_class = 20,
    file_disposition_info_ex_class = 21
};

//! FILE_ATTRIBUTE_TAG_INFO definition from Windows SDK
struct file_attribute_tag_info
{
    DWORD FileAttributes;
    DWORD ReparseTag;
};

//! GetFileInformationByHandleEx signature. Available since Windows Vista.
typedef BOOL (WINAPI GetFileInformationByHandleEx_t)(
    /*__in*/  HANDLE hFile,
    /*__in*/  file_info_by_handle_class FileInformationClass, // the actual type is FILE_INFO_BY_HANDLE_CLASS enum
    /*__out_bcount(dwBufferSize)*/ LPVOID lpFileInformation,
    /*__in*/  DWORD dwBufferSize);

extern GetFileInformationByHandleEx_t* get_file_information_by_handle_ex_api;

//! HANDLE wrapper that automatically closes the handle
struct handle_wrapper
{
    HANDLE handle;

    handle_wrapper() BOOST_NOEXCEPT : handle(INVALID_HANDLE_VALUE) {}
    explicit handle_wrapper(HANDLE h) BOOST_NOEXCEPT : handle(h) {}
    ~handle_wrapper() BOOST_NOEXCEPT
    {
        if (handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }
    BOOST_DELETED_FUNCTION(handle_wrapper(handle_wrapper const&))
    BOOST_DELETED_FUNCTION(handle_wrapper& operator=(handle_wrapper const&))
};

//! Creates a file handle
inline HANDLE create_file_handle(boost::filesystem::path const& p, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile = NULL)
{
    return ::CreateFileW(p.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

} // namespace detail
} // namespace filesystem
} // namespace boost

#endif // BOOST_FILESYSTEM_SRC_WINDOWS_TOOLS_HPP_
