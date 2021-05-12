//  operations.cpp  --------------------------------------------------------------------//

//  Copyright 2002-2009, 2014 Beman Dawes
//  Copyright 2001 Dietmar Kuehl
//  Copyright 2018-2020 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#include "platform_config.hpp"

#include <boost/predef/os/bsd/open.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/system/error_code.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/smart_ptr/scoped_array.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/cstdint.hpp>
#include <boost/assert.hpp>
#include <new> // std::bad_alloc, std::nothrow
#include <limits>
#include <string>
#include <cstddef>
#include <cstdlib>     // for malloc, free
#include <cstring>
#include <cstdio>      // for remove, rename
#if defined(__QNXNTO__)  // see ticket #5355
# include <stdio.h>
#endif
#include <cerrno>

#ifdef BOOST_FILEYSTEM_INCLUDE_IOSTREAM
# include <iostream>
#endif

# ifdef BOOST_POSIX_API

#   include <sys/types.h>
#   include <sys/stat.h>
#   if defined(__wasm)
// WASI does not have statfs or statvfs.
#   elif !defined(__APPLE__) && (!defined(__OpenBSD__) || BOOST_OS_BSD_OPEN >= BOOST_VERSION_NUMBER(4, 4, 0)) && !defined(__ANDROID__) && !defined(__VXWORKS__)
#     include <sys/statvfs.h>
#     define BOOST_STATVFS statvfs
#     define BOOST_STATVFS_F_FRSIZE vfs.f_frsize
#   else
#     ifdef __OpenBSD__
#       include <sys/param.h>
#     elif defined(__ANDROID__)
#       include <sys/vfs.h>
#     endif
#     if !defined(__VXWORKS__)
#       include <sys/mount.h>
#     endif
#     define BOOST_STATVFS statfs
#     define BOOST_STATVFS_F_FRSIZE static_cast<uintmax_t>(vfs.f_bsize)
#   endif
#   include <unistd.h>
#   include <fcntl.h>
#   if _POSIX_C_SOURCE < 200809L
#     include <utime.h>
#   endif
#   include <limits.h>
#   if defined(linux) || defined(__linux) || defined(__linux__)
#     include <sys/utsname.h>
#     include <sys/sendfile.h>
#     include <sys/syscall.h>
#     define BOOST_FILESYSTEM_USE_SENDFILE
#     if defined(__NR_copy_file_range)
#       define BOOST_FILESYSTEM_USE_COPY_FILE_RANGE
#     endif
#     if !defined(BOOST_FILESYSTEM_HAS_STATX) && defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
#       include <linux/stat.h>
#     endif
#   endif

#   if defined(BOOST_FILESYSTEM_HAS_STAT_ST_MTIM)
#     define BOOST_FILESYSTEM_STAT_ST_MTIMENSEC st_mtim.tv_nsec
#   elif defined(BOOST_FILESYSTEM_HAS_STAT_ST_MTIMESPEC)
#     define BOOST_FILESYSTEM_STAT_ST_MTIMENSEC st_mtimespec.tv_nsec
#   elif defined(BOOST_FILESYSTEM_HAS_STAT_ST_MTIMENSEC)
#     define BOOST_FILESYSTEM_STAT_ST_MTIMENSEC st_mtimensec
#   endif

#   if defined(BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIM)
#     define BOOST_FILESYSTEM_STAT_ST_BIRTHTIME st_birthtim.tv_sec
#     define BOOST_FILESYSTEM_STAT_ST_BIRTHTIMENSEC st_birthtim.tv_nsec
#   elif defined(BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIMESPEC)
#     define BOOST_FILESYSTEM_STAT_ST_BIRTHTIME st_birthtimespec.tv_sec
#     define BOOST_FILESYSTEM_STAT_ST_BIRTHTIMENSEC st_birthtimespec.tv_nsec
#   elif defined(BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIMENSEC)
#     define BOOST_FILESYSTEM_STAT_ST_BIRTHTIME st_birthtime
#     define BOOST_FILESYSTEM_STAT_ST_BIRTHTIMENSEC st_birthtimensec
#   endif

# else // BOOST_WINDOWS_API

#   include <boost/winapi/dll.hpp> // get_proc_address, GetModuleHandleW
#   include <cwchar>
#   include <io.h>
#   include <windows.h>
#   include <winnt.h>
#   if defined(__BORLANDC__) || defined(__MWERKS__)
#     if defined(BOOST_BORLANDC)
        using std::time_t;
#     endif
#     include <utime.h>
#   else
#     include <sys/utime.h>
#   endif

#include "windows_tools.hpp"

# endif  // BOOST_WINDOWS_API

#include "error_handling.hpp"

namespace fs = boost::filesystem;
using boost::filesystem::path;
using boost::filesystem::filesystem_error;
using boost::filesystem::perms;
using boost::system::error_code;
using boost::system::system_category;

#if defined(BOOST_POSIX_API)

// At least Mac OS X 10.6 and older doesn't support O_CLOEXEC
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
#define BOOST_FILESYSTEM_HAS_FDATASYNC
#endif

#else // defined(BOOST_POSIX_API)

//  REPARSE_DATA_BUFFER related definitions are found in ntifs.h, which is part of the
//  Windows Device Driver Kit. Since that's inconvenient, the definitions are provided
//  here. See http://msdn.microsoft.com/en-us/library/ms791514.aspx

#if !defined(REPARSE_DATA_BUFFER_HEADER_SIZE)  // mingw winnt.h does provide the defs

#define SYMLINK_FLAG_RELATIVE 1

typedef struct _REPARSE_DATA_BUFFER {
  ULONG  ReparseTag;
  USHORT  ReparseDataLength;
  USHORT  Reserved;
  union {
    struct {
      USHORT  SubstituteNameOffset;
      USHORT  SubstituteNameLength;
      USHORT  PrintNameOffset;
      USHORT  PrintNameLength;
      ULONG  Flags;
      WCHAR  PathBuffer[1];
  /*  Example of distinction between substitute and print names:
        mklink /d ldrive c:\
        SubstituteName: c:\\??\
        PrintName: c:\
  */
     } SymbolicLinkReparseBuffer;
    struct {
      USHORT  SubstituteNameOffset;
      USHORT  SubstituteNameLength;
      USHORT  PrintNameOffset;
      USHORT  PrintNameLength;
      WCHAR  PathBuffer[1];
    } MountPointReparseBuffer;
    struct {
      UCHAR  DataBuffer[1];
    } GenericReparseBuffer;
  };
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;

#define REPARSE_DATA_BUFFER_HEADER_SIZE \
  FIELD_OFFSET(REPARSE_DATA_BUFFER, GenericReparseBuffer)

#endif // !defined(REPARSE_DATA_BUFFER_HEADER_SIZE)

#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 * 1024)
#endif

#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT 0x900a8
#endif

#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK (0xA000000CL)
#endif

// Fallback for MinGW/Cygwin
#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#endif

// Our convenience type for allocating REPARSE_DATA_BUFFER along with sufficient space after it
union reparse_data_buffer
{
  REPARSE_DATA_BUFFER rdb;
  unsigned char storage[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
};

# endif // defined(BOOST_POSIX_API)

//  POSIX/Windows macros  ----------------------------------------------------//

//  Portions of the POSIX and Windows API's are very similar, except for name,
//  order of arguments, and meaning of zero/non-zero returns. The macros below
//  abstract away those differences. They follow Windows naming and order of
//  arguments, and return true to indicate no error occurred. [POSIX naming,
//  order of arguments, and meaning of return were followed initially, but
//  found to be less clear and cause more coding errors.]

# if defined(BOOST_POSIX_API)

#   define BOOST_SET_CURRENT_DIRECTORY(P)(::chdir(P)== 0)
#   define BOOST_CREATE_HARD_LINK(F,T)(::link(T, F)== 0)
#   define BOOST_CREATE_SYMBOLIC_LINK(F,T,Flag)(::symlink(T, F)== 0)
#   define BOOST_REMOVE_DIRECTORY(P)(::rmdir(P)== 0)
#   define BOOST_DELETE_FILE(P)(::unlink(P)== 0)
#   define BOOST_MOVE_FILE(OLD,NEW)(::rename(OLD, NEW)== 0)
#   define BOOST_RESIZE_FILE(P,SZ)(::truncate(P, SZ)== 0)

# else  // BOOST_WINDOWS_API

#   define BOOST_SET_CURRENT_DIRECTORY(P)(::SetCurrentDirectoryW(P)!= 0)
#   define BOOST_CREATE_HARD_LINK(F,T)(create_hard_link_api(F, T, 0)!= 0)
#   define BOOST_CREATE_SYMBOLIC_LINK(F,T,Flag)(create_symbolic_link_api(F, T, Flag)!= 0)
#   define BOOST_REMOVE_DIRECTORY(P)(::RemoveDirectoryW(P)!= 0)
#   define BOOST_DELETE_FILE(P)(::DeleteFileW(P)!= 0)
#   define BOOST_MOVE_FILE(OLD,NEW)(::MoveFileExW(OLD, NEW, MOVEFILE_REPLACE_EXISTING|MOVEFILE_COPY_ALLOWED)!= 0)
#   define BOOST_RESIZE_FILE(P,SZ)(resize_file_api(P, SZ)!= 0)
#   define BOOST_READ_SYMLINK(P,T)

# endif

namespace boost {
namespace filesystem {
namespace detail {

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                        helpers (all operating systems)                               //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace {

// Absolute maximum path length, in bytes, that we're willing to accept from various system calls.
// This value is arbitrary, it is supposed to be a hard limit to avoid memory exhaustion
// in some of the algorithms below in case of some corrupted or maliciously broken filesystem.
BOOST_CONSTEXPR_OR_CONST std::size_t absolute_path_max = 16u * 1024u * 1024u;

fs::file_type query_file_type(const path& p, error_code* ec);

//  general helpers  -----------------------------------------------------------------//

bool is_empty_directory(const path& p, error_code* ec)
{
  fs::directory_iterator itr;
  detail::directory_iterator_construct(itr, p, static_cast< unsigned int >(directory_options::none), ec);
  return itr == fs::directory_iterator();
}

bool not_found_error(int errval) BOOST_NOEXCEPT; // forward declaration

// only called if directory exists
inline bool remove_directory(const path& p) // true if succeeds or not found
{
  return BOOST_REMOVE_DIRECTORY(p.c_str())
    || not_found_error(BOOST_ERRNO);  // mitigate possible file system race. See #11166
}

// only called if file exists
inline bool remove_file(const path& p) // true if succeeds or not found
{
  return BOOST_DELETE_FILE(p.c_str())
    || not_found_error(BOOST_ERRNO);  // mitigate possible file system race. See #11166
}

// called by remove and remove_all_aux
bool remove_file_or_directory(const path& p, fs::file_type type, error_code* ec)
  // return true if file removed, false if not removed
{
  if (type == fs::file_not_found)
  {
    if (ec != 0) ec->clear();
    return false;
  }

  if (type == fs::directory_file
#     ifdef BOOST_WINDOWS_API
      || type == fs::_detail_directory_symlink
#     endif
    )
  {
    if (error(!remove_directory(p) ? BOOST_ERRNO : 0, p, ec,
      "boost::filesystem::remove"))
        return false;
  }
  else
  {
    if (error(!remove_file(p) ? BOOST_ERRNO : 0, p, ec,
      "boost::filesystem::remove"))
        return false;
  }
  return true;
}

uintmax_t remove_all_aux(const path& p, fs::file_type type, error_code* ec)
{
  uintmax_t count = 0u;

  if (type == fs::directory_file)  // but not a directory symlink
  {
    fs::directory_iterator itr;
    fs::detail::directory_iterator_construct(itr, p, static_cast< unsigned int >(directory_options::none), ec);
    if (ec && *ec)
      return count;

    const fs::directory_iterator end_dit;
    while(itr != end_dit)
    {
      fs::file_type tmp_type = query_file_type(itr->path(), ec);
      if (ec != 0 && *ec)
        return count;

      count += remove_all_aux(itr->path(), tmp_type, ec);
      if (ec != 0 && *ec)
        return count;

      fs::detail::directory_iterator_increment(itr, ec);
      if (ec != 0 && *ec)
        return count;
    }
  }

  remove_file_or_directory(p, type, ec);
  if (ec != 0 && *ec)
    return count;

  return ++count;
}

#ifdef BOOST_POSIX_API

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            POSIX-specific helpers                                    //
//                                                                                      //
//--------------------------------------------------------------------------------------//

BOOST_CONSTEXPR_OR_CONST char dot = '.';

#if !defined(BOOST_FILESYSTEM_HAS_STATX) && defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
//! A wrapper for the statx syscall
inline int statx(int dirfd, const char* path, int flags, unsigned int mask, struct ::statx* stx) BOOST_NOEXCEPT
{
  return ::syscall(__NR_statx, dirfd, path, flags, mask, stx);
}
#endif // !defined(BOOST_FILESYSTEM_HAS_STATX) && defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)

struct fd_wrapper
{
  int fd;

  fd_wrapper() BOOST_NOEXCEPT : fd(-1) {}
  explicit fd_wrapper(int fd) BOOST_NOEXCEPT : fd(fd) {}
  ~fd_wrapper() BOOST_NOEXCEPT
  {
    if (fd >= 0)
      ::close(fd);
  }
  BOOST_DELETED_FUNCTION(fd_wrapper(fd_wrapper const&))
  BOOST_DELETED_FUNCTION(fd_wrapper& operator= (fd_wrapper const&))
};

inline bool not_found_error(int errval) BOOST_NOEXCEPT
{
  return errval == ENOENT || errval == ENOTDIR;
}

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)

//! Returns \c true if the two \c statx structures refer to the same file
inline bool equivalent_stat(struct ::statx const& s1, struct ::statx const& s2) BOOST_NOEXCEPT
{
  return s1.stx_dev_major == s2.stx_dev_major && s1.stx_dev_minor == s2.stx_dev_minor && s1.stx_ino == s2.stx_ino;
}

//! Returns file type/access mode from \c statx structure
inline mode_t get_mode(struct ::statx const& st) BOOST_NOEXCEPT
{
  return st.stx_mode;
}

//! Returns file size from \c statx structure
inline uintmax_t get_size(struct ::statx const& st) BOOST_NOEXCEPT
{
  return st.stx_size;
}

#else // defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)

//! Returns \c true if the two \c stat structures refer to the same file
inline bool equivalent_stat(struct ::stat const& s1, struct ::stat const& s2) BOOST_NOEXCEPT
{
  // According to the POSIX stat specs, "The st_ino and st_dev fields
  // taken together uniquely identify the file within the system."
  return s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino;
}

//! Returns file type/access mode from \c stat structure
inline mode_t get_mode(struct ::stat const& st) BOOST_NOEXCEPT
{
  return st.st_mode;
}

//! Returns file size from \c stat structure
inline uintmax_t get_size(struct ::stat const& st) BOOST_NOEXCEPT
{
  return st.st_size;
}

#endif // defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)

typedef int (copy_file_data_t)(int infile, int outfile, uintmax_t size);

//! copy_file implementation that uses read/write loop
int copy_file_data_read_write(int infile, int outfile, uintmax_t size)
{
  BOOST_CONSTEXPR_OR_CONST std::size_t buf_sz = 65536u;
  boost::scoped_array<char> buf(new (std::nothrow) char[buf_sz]);
  if (BOOST_UNLIKELY(!buf.get()))
    return ENOMEM;

  while (true)
  {
    ssize_t sz_read = ::read(infile, buf.get(), buf_sz);
    if (sz_read == 0)
      break;
    if (BOOST_UNLIKELY(sz_read < 0))
    {
      int err = errno;
      if (err == EINTR)
        continue;
      return err;
    }

    // Allow for partial writes - see Advanced Unix Programming (2nd Ed.),
    // Marc Rochkind, Addison-Wesley, 2004, page 94
    for (ssize_t sz_wrote = 0; sz_wrote < sz_read;)
    {
      ssize_t sz = ::write(outfile, buf.get() + sz_wrote, static_cast< std::size_t >(sz_read - sz_wrote));
      if (BOOST_UNLIKELY(sz < 0))
      {
        int err = errno;
        if (err == EINTR)
          continue;
        return err;
      }

      sz_wrote += sz;
    }
  }

  return 0;
}

//! Pointer to the actual implementation of the copy_file_data implementation
copy_file_data_t* copy_file_data = &copy_file_data_read_write;

#if defined(BOOST_FILESYSTEM_USE_SENDFILE)

//! copy_file implementation that uses sendfile loop. Requires sendfile to support file descriptors.
int copy_file_data_sendfile(int infile, int outfile, uintmax_t size)
{
  // sendfile will not send more than this amount of data in one call
  BOOST_CONSTEXPR_OR_CONST std::size_t max_send_size = 0x7ffff000u;
  uintmax_t offset = 0u;
  while (offset < size)
  {
    uintmax_t size_left = size - offset;
    std::size_t size_to_copy = max_send_size;
    if (size_left < static_cast< uintmax_t >(max_send_size))
      size_to_copy = static_cast< std::size_t >(size_left);
    ssize_t sz = ::sendfile(outfile, infile, NULL, size_to_copy);
    if (BOOST_UNLIKELY(sz < 0))
    {
      int err = errno;
      if (err == EINTR)
        continue;
      return err;
    }

    offset += sz;
  }

  return 0;
}

#endif // defined(BOOST_FILESYSTEM_USE_SENDFILE)

#if defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)

//! copy_file implementation that uses copy_file_range loop. Requires copy_file_range to support cross-filesystem copying.
int copy_file_data_copy_file_range(int infile, int outfile, uintmax_t size)
{
  // Although copy_file_range does not document any particular upper limit of one transfer, still use some upper bound to guarantee
  // that size_t is not overflown in case if off_t is larger and the file size does not fit in size_t.
  BOOST_CONSTEXPR_OR_CONST std::size_t max_send_size = 0x7ffff000u;
  uintmax_t offset = 0u;
  while (offset < size)
  {
    uintmax_t size_left = size - offset;
    std::size_t size_to_copy = max_send_size;
    if (size_left < static_cast< uintmax_t >(max_send_size))
      size_to_copy = static_cast< std::size_t >(size_left);
    // Note: Use syscall directly to avoid depending on libc version. copy_file_range is added in glibc 2.27.
    // uClibc-ng does not have copy_file_range as of the time of this writing (the latest uClibc-ng release is 1.0.33).
    loff_t sz = ::syscall(__NR_copy_file_range, infile, (loff_t*)NULL, outfile, (loff_t*)NULL, size_to_copy, (unsigned int)0u);
    if (BOOST_UNLIKELY(sz < 0))
    {
      int err = errno;
      if (err == EINTR)
        continue;
      return err;
    }

    offset += sz;
  }

  return 0;
}

#endif // defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)

#if defined(BOOST_FILESYSTEM_USE_SENDFILE) || defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)

struct copy_file_data_initializer
{
  copy_file_data_initializer()
  {
    struct ::utsname system_info;
    if (BOOST_UNLIKELY(::uname(&system_info) < 0))
      return;

    unsigned int major = 0u, minor = 0u, patch = 0u;
    int count = std::sscanf(system_info.release, "%u.%u.%u", &major, &minor, &patch);
    if (BOOST_UNLIKELY(count < 3))
      return;

    copy_file_data_t* cfd = &copy_file_data_read_write;

#if defined(BOOST_FILESYSTEM_USE_SENDFILE)
    // sendfile started accepting file descriptors as the target in Linux 2.6.33
    if (major > 2u || (major == 2u && (minor > 6u || (minor == 6u && patch >= 33u))))
      cfd = &copy_file_data_sendfile;
#endif

#if defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)
    // Although copy_file_range appeared in Linux 4.5, it did not support cross-filesystem copying until 5.3
    if (major > 5u || (major == 5u && minor >= 3u))
      cfd = &copy_file_data_copy_file_range;
#endif

    copy_file_data = cfd;
  }
}
const copy_file_data_init;

#endif // defined(BOOST_FILESYSTEM_USE_SENDFILE) || defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)

inline fs::file_type query_file_type(const path& p, error_code* ec)
{
  return fs::detail::symlink_status(p, ec).type();
}

# else

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            Windows-specific helpers                                  //
//                                                                                      //
//--------------------------------------------------------------------------------------//

BOOST_CONSTEXPR_OR_CONST wchar_t dot = L'.';

// Windows CE has no environment variables
#if !defined(UNDER_CE)
inline std::wstring wgetenv(const wchar_t* name)
{
  // use a separate buffer since C++03 basic_string is not required to be contiguous
  const DWORD size = ::GetEnvironmentVariableW(name, NULL, 0);
  if (size > 0)
  {
    boost::scoped_array<wchar_t> buf(new wchar_t[size]);
    if (BOOST_LIKELY(::GetEnvironmentVariableW(name, buf.get(), size) > 0))
      return std::wstring(buf.get());
  }

  return std::wstring();
}
#endif // !defined(UNDER_CE)

inline bool not_found_error(int errval) BOOST_NOEXCEPT
{
  return errval == ERROR_FILE_NOT_FOUND
    || errval == ERROR_PATH_NOT_FOUND
    || errval == ERROR_INVALID_NAME  // "tools/jam/src/:sys:stat.h", "//foo"
    || errval == ERROR_INVALID_DRIVE  // USB card reader with no card inserted
    || errval == ERROR_NOT_READY  // CD/DVD drive with no disc inserted
    || errval == ERROR_INVALID_PARAMETER  // ":sys:stat.h"
    || errval == ERROR_BAD_PATHNAME  // "//nosuch" on Win64
    || errval == ERROR_BAD_NETPATH;  // "//nosuch" on Win32
}

// these constants come from inspecting some Microsoft sample code
inline std::time_t to_time_t(const FILETIME & ft) BOOST_NOEXCEPT
{
  uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  t -= 116444736000000000ull;
  t /= 10000000u;
  return static_cast<std::time_t>(t);
}

inline void to_FILETIME(std::time_t t, FILETIME & ft) BOOST_NOEXCEPT
{
  uint64_t temp = t;
  temp *= 10000000u;
  temp += 116444736000000000ull;
  ft.dwLowDateTime = static_cast<DWORD>(temp);
  ft.dwHighDateTime = static_cast<DWORD>(temp >> 32);
}

// Thanks to Jeremy Maitin-Shepard for much help and for permission to
// base the equivalent()implementation on portions of his
// file-equivalence-win32.cpp experimental code.

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
  BOOST_DELETED_FUNCTION(handle_wrapper& operator= (handle_wrapper const&))
};

inline HANDLE create_file_handle(const path& p, DWORD dwDesiredAccess,
  DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
  DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
  HANDLE hTemplateFile)
{
  return ::CreateFileW(p.c_str(), dwDesiredAccess, dwShareMode,
    lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes,
    hTemplateFile);
}

bool is_reparse_point_a_symlink(const path& p)
{
  handle_wrapper h(create_file_handle(p, 0,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL));
  if (h.handle == INVALID_HANDLE_VALUE)
    return false;

  boost::scoped_ptr<reparse_data_buffer> buf(new reparse_data_buffer);

  // Query the reparse data
  DWORD dwRetLen = 0u;
  BOOL result = ::DeviceIoControl(h.handle, FSCTL_GET_REPARSE_POINT, NULL, 0, buf.get(),
    sizeof(*buf), &dwRetLen, NULL);
  if (!result) return false;

  return buf->rdb.ReparseTag == IO_REPARSE_TAG_SYMLINK
      // Issue 9016 asked that NTFS directory junctions be recognized as directories.
      // That is equivalent to recognizing them as symlinks, and then the normal symlink
      // mechanism will take care of recognizing them as directories.
      //
      // Directory junctions are very similar to symlinks, but have some performance
      // and other advantages over symlinks. They can be created from the command line
      // with "mklink /j junction-name target-path".
    || buf->rdb.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT;  // aka "directory junction" or "junction"
}

inline std::size_t get_full_path_name(
  const path& src, std::size_t len, wchar_t* buf, wchar_t** p)
{
  return static_cast<std::size_t>(
    ::GetFullPathNameW(src.c_str(), static_cast<DWORD>(len), buf, p));
}

fs::file_status process_status_failure(const path& p, error_code* ec)
{
  int errval(::GetLastError());
  if (ec != 0)                             // always report errval, even though some
    ec->assign(errval, system_category());   // errval values are not status_errors

  if (not_found_error(errval))
  {
    return fs::file_status(fs::file_not_found, fs::no_perms);
  }
  else if (errval == ERROR_SHARING_VIOLATION)
  {
    return fs::file_status(fs::type_unknown);
  }
  if (ec == 0)
    BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::status",
      p, error_code(errval, system_category())));
  return fs::file_status(fs::status_error);
}

//  differs from symlink_status() in that directory symlinks are reported as
//  _detail_directory_symlink, as required on Windows by remove() and its helpers.
fs::file_type query_file_type(const path& p, error_code* ec)
{
  DWORD attr(::GetFileAttributesW(p.c_str()));
  if (attr == 0xFFFFFFFF)
  {
    return process_status_failure(p, ec).type();
  }

  if (ec != 0) ec->clear();

  if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
  {
    if (is_reparse_point_a_symlink(p))
      return (attr & FILE_ATTRIBUTE_DIRECTORY)
        ? fs::_detail_directory_symlink
        : fs::symlink_file;
    return fs::reparse_file;
  }

  return (attr & FILE_ATTRIBUTE_DIRECTORY)
    ? fs::directory_file
    : fs::regular_file;
}

inline BOOL resize_file_api(const wchar_t* p, uintmax_t size)
{
  handle_wrapper h(CreateFileW(p, GENERIC_WRITE, 0, 0, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, 0));
  LARGE_INTEGER sz;
  sz.QuadPart = size;
  return h.handle != INVALID_HANDLE_VALUE
    && ::SetFilePointerEx(h.handle, sz, 0, FILE_BEGIN)
    && ::SetEndOfFile(h.handle);
}

//  Windows kernel32.dll functions that may or may not be present
//  must be accessed through pointers

typedef BOOL (WINAPI *PtrCreateHardLinkW)(
  /*__in*/       LPCWSTR lpFileName,
  /*__in*/       LPCWSTR lpExistingFileName,
  /*__reserved*/ LPSECURITY_ATTRIBUTES lpSecurityAttributes
 );

PtrCreateHardLinkW create_hard_link_api = PtrCreateHardLinkW(
  boost::winapi::get_proc_address(
    boost::winapi::GetModuleHandleW(L"kernel32.dll"), "CreateHardLinkW"));

typedef BOOLEAN (WINAPI *PtrCreateSymbolicLinkW)(
  /*__in*/ LPCWSTR lpSymlinkFileName,
  /*__in*/ LPCWSTR lpTargetFileName,
  /*__in*/ DWORD dwFlags
 );

PtrCreateSymbolicLinkW create_symbolic_link_api = PtrCreateSymbolicLinkW(
  boost::winapi::get_proc_address(
    boost::winapi::GetModuleHandleW(L"kernel32.dll"), "CreateSymbolicLinkW"));

#endif

//#ifdef BOOST_WINDOWS_API
//
//
//  inline bool get_free_disk_space(const std::wstring& ph,
//    PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free)
//    { return ::GetDiskFreeSpaceExW(ph.c_str(), avail, total, free)!= 0; }
//
//#endif

} // unnamed namespace
} // namespace detail

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                operations functions declared in operations.hpp                       //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace detail {

BOOST_FILESYSTEM_DECL bool possible_large_file_size_support()
{
# ifdef BOOST_POSIX_API
  typedef struct stat struct_stat;
  return sizeof(struct_stat().st_size) > 4;
# else
  return true;
# endif
}

BOOST_FILESYSTEM_DECL
path absolute(const path& p, const path& base, system::error_code* ec)
{
//  if ( p.empty() || p.is_absolute() )
//    return p;
//  //  recursively calling absolute is sub-optimal, but is simple
//  path abs_base(base.is_absolute() ? base : absolute(base));
//# ifdef BOOST_WINDOWS_API
//  if (p.has_root_directory())
//    return abs_base.root_name() / p;
//  //  !p.has_root_directory
//  if (p.has_root_name())
//    return p.root_name()
//      / abs_base.root_directory() / abs_base.relative_path() / p.relative_path();
//  //  !p.has_root_name()
//# endif
//  return abs_base / p;

  if (ec != 0)
    ec->clear();

  //  recursively calling absolute is sub-optimal, but is sure and simple
  path abs_base = base;
  if (!base.is_absolute())
  {
    if (ec)
    {
      abs_base = absolute(base, *ec);
      if (*ec)
        return path();
    }
    else
    {
      abs_base = absolute(base);
    }
  }

  //  store expensive to compute values that are needed multiple times
  path p_root_name (p.root_name());
  path base_root_name (abs_base.root_name());
  path p_root_directory (p.root_directory());

  if (p.empty())
    return abs_base;

  if (!p_root_name.empty())  // p.has_root_name()
  {
    if (p_root_directory.empty())  // !p.has_root_directory()
      return p_root_name / abs_base.root_directory()
      / abs_base.relative_path() / p.relative_path();
    // p is absolute, so fall through to return p at end of block
  }
  else if (!p_root_directory.empty())  // p.has_root_directory()
  {
#   ifdef BOOST_POSIX_API
    // POSIX can have root name it it is a network path
    if (base_root_name.empty())   // !abs_base.has_root_name()
      return p;
#   endif
    return base_root_name / p;
  }
  else
  {
    return abs_base / p;
  }

  return p;  // p.is_absolute() is true
}

BOOST_FILESYSTEM_DECL
path canonical(const path& p, const path& base, system::error_code* ec)
{
  if (ec != 0)
    ec->clear();

  path result;
  path source = p;
  if (!p.is_absolute())
  {
    source = detail::absolute(p, base, ec);
    if (ec && *ec)
      return result;
  }

  path root(source.root_path());

  system::error_code local_ec;
  file_status stat (status(source, local_ec));

  if (stat.type() == fs::file_not_found)
  {
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error(
        "boost::filesystem::canonical", source,
        error_code(system::errc::no_such_file_or_directory, system::generic_category())));
    ec->assign(system::errc::no_such_file_or_directory, system::generic_category());
    return result;
  }
  else if (local_ec)
  {
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error(
        "boost::filesystem::canonical", source, local_ec));
    *ec = local_ec;
    return result;
  }

  bool scan = true;
  while (scan)
  {
    scan = false;
    result.clear();
    for (path::iterator itr = source.begin(); itr != source.end(); ++itr)
    {
      if (*itr == dot_path())
        continue;
      if (*itr == dot_dot_path())
      {
        if (result != root)
          result.remove_filename();
        continue;
      }

      result /= *itr;

      // If we don't have an absolute path yet then don't check symlink status.
      // This avoids checking "C:" which is "the current directory on drive C"
      // and hence not what we want to check/resolve here.
      if (!result.is_absolute())
          continue;

      bool is_sym (is_symlink(detail::symlink_status(result, ec)));
      if (ec && *ec)
        return path();

      if (is_sym)
      {
        path link(detail::read_symlink(result, ec));
        if (ec && *ec)
          return path();
        result.remove_filename();

        if (link.is_absolute())
        {
          for (++itr; itr != source.end(); ++itr)
            link /= *itr;
          source = link;
        }
        else // link is relative
        {
          path new_source(result);
          new_source /= link;
          for (++itr; itr != source.end(); ++itr)
            new_source /= *itr;
          source = new_source;
        }
        scan = true;  // symlink causes scan to be restarted
        break;
      }
    }
  }
  BOOST_ASSERT_MSG(result.is_absolute(), "canonical() implementation error; please report");
  return result;
}

BOOST_FILESYSTEM_DECL
void copy(const path& from, const path& to, unsigned int options, system::error_code* ec)
{
  BOOST_ASSERT((((options & static_cast< unsigned int >(copy_options::overwrite_existing)) != 0u) +
    ((options & static_cast< unsigned int >(copy_options::skip_existing)) != 0u) +
    ((options & static_cast< unsigned int >(copy_options::update_existing)) != 0u)) <= 1);

  BOOST_ASSERT((((options & static_cast< unsigned int >(copy_options::copy_symlinks)) != 0u) +
    ((options & static_cast< unsigned int >(copy_options::skip_symlinks)) != 0u)) <= 1);

  BOOST_ASSERT((((options & static_cast< unsigned int >(copy_options::directories_only)) != 0u) +
    ((options & static_cast< unsigned int >(copy_options::create_symlinks)) != 0u) +
    ((options & static_cast< unsigned int >(copy_options::create_hard_links)) != 0u)) <= 1);

  file_status from_stat;
  if ((options & (static_cast< unsigned int >(copy_options::copy_symlinks) |
    static_cast< unsigned int >(copy_options::skip_symlinks) |
    static_cast< unsigned int >(copy_options::create_symlinks))) != 0u)
  {
    from_stat = detail::symlink_status(from, ec);
  }
  else
  {
    from_stat = detail::status(from, ec);
  }

  if (ec && *ec)
    return;

  if (!exists(from_stat))
  {
    emit_error(BOOST_ERROR_FILE_NOT_FOUND, from, to, ec, "boost::filesystem::copy");
    return;
  }

  if (is_symlink(from_stat))
  {
    if ((options & static_cast< unsigned int >(copy_options::skip_symlinks)) != 0u)
      return;

    if ((options & static_cast< unsigned int >(copy_options::copy_symlinks)) == 0u)
      goto fail;

    detail::copy_symlink(from, to, ec);
  }
  else if (is_regular_file(from_stat))
  {
    if ((options & static_cast< unsigned int >(copy_options::directories_only)) != 0u)
      return;

    if ((options & static_cast< unsigned int >(copy_options::create_symlinks)) != 0u)
    {
      const path* pfrom = &from;
      path relative_from;
      if (!from.is_absolute())
      {
        // Try to generate a relative path from the target location to the original file
        path cur_dir = detail::current_path(ec);
        if (ec && *ec)
          return;
        path abs_from = detail::absolute(from.parent_path(), cur_dir, ec);
        if (ec && *ec)
          return;
        path abs_to = to.parent_path();
        if (!abs_to.is_absolute())
        {
          abs_to = detail::absolute(abs_to, cur_dir, ec);
          if (ec && *ec)
            return;
        }
        relative_from = detail::relative(abs_from, abs_to, ec);
        if (ec && *ec)
          return;
        if (relative_from != dot_path())
          relative_from /= from.filename();
        else
          relative_from = from.filename();
        pfrom = &relative_from;
      }
      detail::create_symlink(*pfrom, to, ec);
      return;
    }

    if ((options & static_cast< unsigned int >(copy_options::create_hard_links)) != 0u)
    {
      detail::create_hard_link(from, to, ec);
      return;
    }

    error_code local_ec;
    file_status to_stat;
    if ((options & (static_cast< unsigned int >(copy_options::skip_symlinks) |
      static_cast< unsigned int >(copy_options::create_symlinks))) != 0u)
    {
      to_stat = detail::symlink_status(to, &local_ec);
    }
    else
    {
      to_stat = detail::status(to, &local_ec);
    }

    // Note: local_ec may be set by (symlink_)status() even in some non-fatal situations, e.g. when the file does not exist.
    //       OTOH, when it returns status_error, then a real error have happened and it must have set local_ec.
    if (to_stat.type() == fs::status_error)
    {
      if (!ec)
        BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::copy", from, to, local_ec));
      *ec = local_ec;
      return;
    }

    if (is_directory(to_stat))
      detail::copy_file(from, to / from.filename(), options, ec);
    else
      detail::copy_file(from, to, options, ec);
  }
  else if (is_directory(from_stat))
  {
    error_code local_ec;
    if ((options & static_cast< unsigned int >(copy_options::create_symlinks)) != 0u)
    {
      local_ec = make_error_code(system::errc::is_a_directory);
      if (!ec)
        BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::copy", from, to, local_ec));
      *ec = local_ec;
      return;
    }

    file_status to_stat;
    if ((options & (static_cast< unsigned int >(copy_options::skip_symlinks) |
      static_cast< unsigned int >(copy_options::create_symlinks))) != 0u)
    {
      to_stat = detail::symlink_status(to, &local_ec);
    }
    else
    {
      to_stat = detail::status(to, &local_ec);
    }

    // Note: ec may be set by (symlink_)status() even in some non-fatal situations, e.g. when the file does not exist.
    //       OTOH, when it returns status_error, then a real error have happened and it must have set local_ec.
    if (to_stat.type() == fs::status_error)
    {
      if (!ec)
        BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::copy", from, to, local_ec));
      *ec = local_ec;
      return;
    }

    if (!exists(to_stat))
    {
      detail::create_directory(to, &from, ec);
      if (ec && *ec)
        return;
    }

    if ((options & static_cast< unsigned int >(copy_options::recursive)) != 0u || options == 0u)
    {
      fs::directory_iterator itr;
      detail::directory_iterator_construct(itr, from, static_cast< unsigned int >(directory_options::none), ec);
      if (ec && *ec)
        return;

      const fs::directory_iterator end_dit;
      while (itr != end_dit)
      {
        path const& p = itr->path();
        // Set _detail_recursing flag so that we don't recurse more than for one level deeper into the directory if options are copy_options::none
        detail::copy(p, to / p.filename(), options | static_cast< unsigned int >(copy_options::_detail_recursing), ec);
        if (ec && *ec)
          return;

        detail::directory_iterator_increment(itr, ec);
        if (ec && *ec)
          return;
      }
    }
  }
  else
  {
  fail:
    emit_error(BOOST_ERROR_NOT_SUPPORTED, from, to, ec, "boost::filesystem::copy");
  }
}

BOOST_FILESYSTEM_DECL
bool copy_file(const path& from, const path& to, unsigned int options, error_code* ec)
{
  BOOST_ASSERT((((options & static_cast< unsigned int >(copy_options::overwrite_existing)) != 0u) +
    ((options & static_cast< unsigned int >(copy_options::skip_existing)) != 0u) +
    ((options & static_cast< unsigned int >(copy_options::update_existing)) != 0u)) <= 1);

  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

  int err = 0;

  // Note: Declare fd_wrappers here so that errno is not clobbered by close() that may be called in fd_wrapper destructors
  fd_wrapper infile, outfile;

  while (true)
  {
    infile.fd = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (BOOST_UNLIKELY(infile.fd < 0))
    {
      err = errno;
      if (err == EINTR)
        continue;

    fail:
      emit_error(err, from, to, ec, "boost::filesystem::copy_file");
      return false;
    }

    break;
  }

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  unsigned int statx_data_mask = STATX_TYPE | STATX_MODE | STATX_INO | STATX_SIZE;
  if ((options & static_cast< unsigned int >(copy_options::update_existing)) != 0u)
    statx_data_mask |= STATX_MTIME;

  struct ::statx from_stat;
  if (BOOST_UNLIKELY(statx(infile.fd, "", AT_EMPTY_PATH | AT_NO_AUTOMOUNT, statx_data_mask, &from_stat) < 0))
  {
  fail_errno:
    err = errno;
    goto fail;
  }

  if (BOOST_UNLIKELY((from_stat.stx_mask & statx_data_mask) != statx_data_mask))
  {
    err = ENOSYS;
    goto fail;
  }
#else
  struct ::stat from_stat;
  if (BOOST_UNLIKELY(::fstat(infile.fd, &from_stat) != 0))
  {
  fail_errno:
    err = errno;
    goto fail;
  }
#endif

  const mode_t from_mode = get_mode(from_stat);
  if (BOOST_UNLIKELY(!S_ISREG(from_mode)))
  {
    err = ENOSYS;
    goto fail;
  }

  mode_t to_mode = from_mode;
#if !defined(__wasm)
  // Enable writing for the newly created files. Having write permission set is important e.g. for NFS,
  // which checks the file permission on the server, even if the client's file descriptor supports writing.
  to_mode |= S_IWUSR;
#endif
  int oflag = O_WRONLY | O_CLOEXEC;

  if ((options & static_cast< unsigned int >(copy_options::update_existing)) != 0u)
  {
    // Try opening the existing file without truncation to test the modification time later
    while (true)
    {
      outfile.fd = ::open(to.c_str(), oflag, to_mode);
      if (outfile.fd < 0)
      {
        err = errno;
        if (err == EINTR)
          continue;

        if (err == ENOENT)
          goto create_outfile;

        goto fail;
      }

      break;
    }
  }
  else
  {
  create_outfile:
    oflag |= O_CREAT | O_TRUNC;
    if (((options & static_cast< unsigned int >(copy_options::overwrite_existing)) == 0u ||
      (options & static_cast< unsigned int >(copy_options::skip_existing)) != 0u) &&
      (options & static_cast< unsigned int >(copy_options::update_existing)) == 0u)
    {
      oflag |= O_EXCL;
    }

    while (true)
    {
      outfile.fd = ::open(to.c_str(), oflag, to_mode);
      if (outfile.fd < 0)
      {
        err = errno;
        if (err == EINTR)
          continue;

        if (err == EEXIST && (options & static_cast< unsigned int >(copy_options::skip_existing)) != 0u)
          return false;

        goto fail;
      }

      break;
    }
  }

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  statx_data_mask = STATX_TYPE | STATX_MODE | STATX_INO;
  if ((oflag & O_TRUNC) == 0)
  {
    // O_TRUNC is not set if copy_options::update_existing is set and an existing file was opened.
    statx_data_mask |= STATX_MTIME;
  }

  struct ::statx to_stat;
  if (BOOST_UNLIKELY(statx(outfile.fd, "", AT_EMPTY_PATH | AT_NO_AUTOMOUNT, statx_data_mask, &to_stat) < 0))
    goto fail_errno;

  if (BOOST_UNLIKELY((to_stat.stx_mask & statx_data_mask) != statx_data_mask))
  {
    err = ENOSYS;
    goto fail;
  }
#else
  struct ::stat to_stat;
  if (BOOST_UNLIKELY(::fstat(outfile.fd, &to_stat) != 0))
    goto fail_errno;
#endif

  to_mode = get_mode(to_stat);
  if (BOOST_UNLIKELY(!S_ISREG(to_mode)))
  {
    err = ENOSYS;
    goto fail;
  }

  if (BOOST_UNLIKELY(detail::equivalent_stat(from_stat, to_stat)))
  {
    err = EEXIST;
    goto fail;
  }

  if ((oflag & O_TRUNC) == 0)
  {
    // O_TRUNC is not set if copy_options::update_existing is set and an existing file was opened.
    // We need to check the last write times.
#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
    if (from_stat.stx_mtime.tv_sec < to_stat.stx_mtime.tv_sec || (from_stat.stx_mtime.tv_sec == to_stat.stx_mtime.tv_sec && from_stat.stx_mtime.tv_nsec <= to_stat.stx_mtime.tv_nsec))
      return false;
#elif defined(BOOST_FILESYSTEM_STAT_ST_MTIMENSEC)
    // Modify time is available with nanosecond precision.
    if (from_stat.st_mtime < to_stat.st_mtime || (from_stat.st_mtime == to_stat.st_mtime && from_stat.BOOST_FILESYSTEM_STAT_ST_MTIMENSEC <= to_stat.BOOST_FILESYSTEM_STAT_ST_MTIMENSEC))
      return false;
#else
    if (from_stat.st_mtime <= to_stat.st_mtime)
      return false;
#endif

    if (BOOST_UNLIKELY(::ftruncate(outfile.fd, 0) != 0))
      goto fail_errno;
  }

  err = detail::copy_file_data(infile.fd, outfile.fd, get_size(from_stat));
  if (BOOST_UNLIKELY(err != 0))
    goto fail; // err already contains the error code

#if !defined(__wasm)
  // If we created a new file with an explicitly added S_IWUSR permission,
  // we may need to update its mode bits to match the source file.
  if (to_mode != from_mode)
  {
    if (BOOST_UNLIKELY(::fchmod(outfile.fd, from_mode) != 0))
      goto fail_errno;
  }
#endif

  // Note: Use fsync/fdatasync followed by close to avoid dealing with the possibility of close failing with EINTR.
  // Even if close fails, including with EINTR, most operating systems (presumably, except HP-UX) will close the
  // file descriptor upon its return. This means that if an error happens later, when the OS flushes data to the
  // underlying media, this error will go unnoticed and we have no way to receive it from close. Calling fsync/fdatasync
  // ensures that all data have been written, and even if close fails for some unfathomable reason, we don't really
  // care at that point.
#if defined(BOOST_FILESYSTEM_HAS_FDATASYNC)
  err = ::fdatasync(outfile.fd);
#else
  err = ::fsync(outfile.fd);
#endif
  if (BOOST_UNLIKELY(err != 0))
    goto fail_errno;

  return true;

#else // defined(BOOST_POSIX_API)

  bool fail_if_exists = (options & static_cast< unsigned int >(copy_options::overwrite_existing)) == 0u ||
    (options & static_cast< unsigned int >(copy_options::skip_existing)) != 0u;

  if ((options & static_cast< unsigned int >(copy_options::update_existing)) != 0u)
  {
    // Create handle_wrappers here so that CloseHandle calls don't clobber error code returned by GetLastError
    handle_wrapper hw_from, hw_to;

    hw_from.handle = create_file_handle(from.c_str(), 0,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);

    FILETIME lwt_from;
    if (hw_from.handle == INVALID_HANDLE_VALUE)
    {
    fail_last_error:
      DWORD err = ::GetLastError();
      emit_error(err, from, to, ec, "boost::filesystem::copy_file");
      return false;
    }

    if (!::GetFileTime(hw_from.handle, 0, 0, &lwt_from))
      goto fail_last_error;

    hw_to.handle = create_file_handle(to.c_str(), 0,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);

    if (hw_to.handle != INVALID_HANDLE_VALUE)
    {
      FILETIME lwt_to;
      if (!::GetFileTime(hw_to.handle, 0, 0, &lwt_to))
        goto fail_last_error;

      ULONGLONG tfrom = (static_cast< ULONGLONG >(lwt_from.dwHighDateTime) << 32) | static_cast< ULONGLONG >(lwt_from.dwLowDateTime);
      ULONGLONG tto = (static_cast< ULONGLONG >(lwt_to.dwHighDateTime) << 32) | static_cast< ULONGLONG >(lwt_to.dwLowDateTime);
      if (tfrom <= tto)
        return false;
    }

    fail_if_exists = false;
  }

  BOOL res = ::CopyFileW(from.c_str(), to.c_str(), fail_if_exists);
  if (!res)
  {
    DWORD err = ::GetLastError();
    if ((err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) && (options & static_cast< unsigned int >(copy_options::skip_existing)) != 0u)
      return false;
    emit_error(err, from, to, ec, "boost::filesystem::copy_file");
    return false;
  }

  return true;

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
void copy_symlink(const path& existing_symlink, const path& new_symlink, system::error_code* ec)
{
  path p(read_symlink(existing_symlink, ec));
  if (ec && *ec)
    return;
  create_symlink(p, new_symlink, ec);
}

BOOST_FILESYSTEM_DECL
bool create_directories(const path& p, system::error_code* ec)
{
  if (p.empty())
  {
    if (!ec)
    {
      BOOST_FILESYSTEM_THROW(filesystem_error(
        "boost::filesystem::create_directories", p,
        system::errc::make_error_code(system::errc::invalid_argument)));
    }
    ec->assign(system::errc::invalid_argument, system::generic_category());
    return false;
  }

  if (p.filename_is_dot() || p.filename_is_dot_dot())
    return create_directories(p.parent_path(), ec);

  error_code local_ec;
  file_status p_status = detail::status(p, &local_ec);

  if (p_status.type() == directory_file)
  {
    if (ec)
      ec->clear();
    return false;
  }
  else if (BOOST_UNLIKELY(p_status.type() == status_error))
  {
    if (!ec)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::create_directories", p, local_ec));
    *ec = local_ec;
    return false;
  }

  path parent = p.parent_path();
  BOOST_ASSERT_MSG(parent != p, "internal error: p == p.parent_path()");
  if (!parent.empty())
  {
    // determine if the parent exists
    file_status parent_status = detail::status(parent, &local_ec);

    // if the parent does not exist, create the parent
    if (parent_status.type() == file_not_found)
    {
      create_directories(parent, local_ec);
      if (BOOST_UNLIKELY(!!local_ec))
      {
      parent_fail_local_ec:
        if (!ec)
          BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::create_directories", parent, local_ec));
        *ec = local_ec;
        return false;
      }
    }
    else if (BOOST_UNLIKELY(parent_status.type() == status_error))
    {
      goto parent_fail_local_ec;
    }
  }

  // create the directory
  return create_directory(p, NULL, ec);
}

BOOST_FILESYSTEM_DECL
bool create_directory(const path& p, const path* existing, error_code* ec)
{
  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

  mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO;
  if (existing)
  {
#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
    struct ::statx existing_stat;
    if (BOOST_UNLIKELY(statx(AT_FDCWD, existing->c_str(), AT_NO_AUTOMOUNT, STATX_TYPE | STATX_MODE, &existing_stat) < 0))
    {
      emit_error(errno, p, *existing, ec, "boost::filesystem::create_directory");
      return false;
    }

    if (BOOST_UNLIKELY((existing_stat.stx_mask & (STATX_TYPE | STATX_MODE)) != (STATX_TYPE | STATX_MODE)))
    {
      emit_error(BOOST_ERROR_NOT_SUPPORTED, p, *existing, ec, "boost::filesystem::create_directory");
      return false;
    }
#else
    struct ::stat existing_stat;
    if (::stat(existing->c_str(), &existing_stat) < 0)
    {
      emit_error(errno, p, *existing, ec, "boost::filesystem::create_directory");
      return false;
    }
#endif

    const mode_t existing_mode = get_mode(existing_stat);
    if (!S_ISDIR(existing_mode))
    {
      emit_error(ENOTDIR, p, *existing, ec, "boost::filesystem::create_directory");
      return false;
    }

    mode = existing_mode;
  }

  if (::mkdir(p.c_str(), mode) == 0)
    return true;

#else // defined(BOOST_POSIX_API)

  BOOL res;
  if (existing)
    res = ::CreateDirectoryExW(existing->c_str(), p.c_str(), NULL);
  else
    res = ::CreateDirectoryW(p.c_str(), NULL);

  if (res)
    return true;

#endif // defined(BOOST_POSIX_API)

  //  attempt to create directory failed
  err_t errval = BOOST_ERRNO;  // save reason for failure
  error_code dummy;

  if (is_directory(p, dummy))
    return false;

  //  attempt to create directory failed && it doesn't already exist
  emit_error(errval, p, ec, "boost::filesystem::create_directory");
  return false;
}

// Deprecated, to be removed in a future release
BOOST_FILESYSTEM_DECL
void copy_directory(const path& from, const path& to, system::error_code* ec)
{
  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  int err;
  struct ::statx from_stat;
  if (BOOST_UNLIKELY(statx(AT_FDCWD, from.c_str(), AT_NO_AUTOMOUNT, STATX_TYPE | STATX_MODE, &from_stat) < 0))
  {
  fail_errno:
    err = errno;
  fail:
    emit_error(err, from, to, ec, "boost::filesystem::copy_directory");
    return;
  }

  if (BOOST_UNLIKELY((from_stat.stx_mask & (STATX_TYPE | STATX_MODE)) != (STATX_TYPE | STATX_MODE)))
  {
    err = BOOST_ERROR_NOT_SUPPORTED;
    goto fail;
  }
#else
  struct ::stat from_stat;
  if (BOOST_UNLIKELY(::stat(from.c_str(), &from_stat) < 0))
  {
  fail_errno:
    emit_error(errno, from, to, ec, "boost::filesystem::copy_directory");
    return;
  }
#endif

  if (BOOST_UNLIKELY(::mkdir(to.c_str(), get_mode(from_stat)) < 0))
    goto fail_errno;

#else // defined(BOOST_POSIX_API)

  if (BOOST_UNLIKELY(!::CreateDirectoryExW(from.c_str(), to.c_str(), 0)))
    emit_error(BOOST_ERRNO, from, to, ec, "boost::filesystem::copy_directory");

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
void create_directory_symlink(const path& to, const path& from, system::error_code* ec)
{
#if defined(BOOST_WINDOWS_API)
  // see if actually supported by Windows runtime dll
  if (error(!create_symbolic_link_api ? BOOST_ERROR_NOT_SUPPORTED : 0, to, from, ec,
      "boost::filesystem::create_directory_symlink"))
    return;
#endif

  error(!BOOST_CREATE_SYMBOLIC_LINK(from.c_str(), to.c_str(),
    SYMBOLIC_LINK_FLAG_DIRECTORY) ? BOOST_ERRNO : 0,
    to, from, ec, "boost::filesystem::create_directory_symlink");
}

BOOST_FILESYSTEM_DECL
void create_hard_link(const path& to, const path& from, error_code* ec)
{
#if defined(BOOST_WINDOWS_API)
  // see if actually supported by Windows runtime dll
  if (error(!create_hard_link_api ? BOOST_ERROR_NOT_SUPPORTED : 0, to, from, ec,
      "boost::filesystem::create_hard_link"))
    return;
#endif

  error(!BOOST_CREATE_HARD_LINK(from.c_str(), to.c_str()) ? BOOST_ERRNO : 0, to, from, ec,
    "boost::filesystem::create_hard_link");
}

BOOST_FILESYSTEM_DECL
void create_symlink(const path& to, const path& from, error_code* ec)
{
#if defined(BOOST_WINDOWS_API)
  // see if actually supported by Windows runtime dll
  if (error(!create_symbolic_link_api ? BOOST_ERROR_NOT_SUPPORTED : 0, to, from, ec,
      "boost::filesystem::create_symlink"))
    return;
#endif

  error(!BOOST_CREATE_SYMBOLIC_LINK(from.c_str(), to.c_str(), 0) ? BOOST_ERRNO : 0,
    to, from, ec, "boost::filesystem::create_symlink");
}

BOOST_FILESYSTEM_DECL
path current_path(error_code* ec)
{
# if defined(__wasm)
  emit_error(BOOST_ERROR_NOT_SUPPORTED, ec, "boost::filesystem::current_path");
  return path();
# elif defined(BOOST_POSIX_API)
  struct local
  {
    static bool getcwd_error(error_code* ec)
    {
      const int err = errno;
      return error((err != ERANGE
          // bug in some versions of the Metrowerks C lib on the Mac: wrong errno set
#   if defined(__MSL__) && (defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__))
          && err != 0
#   endif
        ) ? err : 0, ec, "boost::filesystem::current_path");
    }
  };

  path cur;
  char small_buf[1024];
  const char* p = ::getcwd(small_buf, sizeof(small_buf));
  if (BOOST_LIKELY(!!p))
  {
    cur = p;
    if (ec != 0) ec->clear();
  }
  else if (BOOST_LIKELY(!local::getcwd_error(ec)))
  {
    for (std::size_t path_max = sizeof(small_buf);; path_max *= 2u) // loop 'til buffer large enough
    {
      if (BOOST_UNLIKELY(path_max > absolute_path_max))
      {
        emit_error(ENAMETOOLONG, ec, "boost::filesystem::current_path");
        break;
      }

      boost::scoped_array<char> buf(new char[path_max]);
      p = ::getcwd(buf.get(), path_max);
      if (BOOST_LIKELY(!!p))
      {
        cur = buf.get();
        if (ec != 0)
          ec->clear();
        break;
      }
      else if (BOOST_UNLIKELY(local::getcwd_error(ec)))
      {
        break;
      }
    }
  }

  return cur;

# elif defined(UNDER_CE)
  // Windows CE has no current directory, so everything's relative to the root of the directory tree
  return L"\\";
# else
  DWORD sz;
  if ((sz = ::GetCurrentDirectoryW(0, NULL)) == 0)sz = 1;
  boost::scoped_array<path::value_type> buf(new path::value_type[sz]);
  error(::GetCurrentDirectoryW(sz, buf.get()) == 0 ? BOOST_ERRNO : 0, ec,
    "boost::filesystem::current_path");
  return path(buf.get());
# endif
}


BOOST_FILESYSTEM_DECL
void current_path(const path& p, system::error_code* ec)
{
# if defined(UNDER_CE) || defined(__wasm)
  emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::current_path");
# else
  error(!BOOST_SET_CURRENT_DIRECTORY(p.c_str()) ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::current_path");
# endif
}

BOOST_FILESYSTEM_DECL
bool equivalent(const path& p1, const path& p2, system::error_code* ec)
{
#if defined(BOOST_POSIX_API)

  // p2 is done first, so any error reported is for p1
#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  struct ::statx s2;
  int e2 = statx(AT_FDCWD, p2.c_str(), AT_NO_AUTOMOUNT, STATX_INO, &s2);
  if (BOOST_LIKELY(e2 == 0))
  {
    if (BOOST_UNLIKELY((s2.stx_mask & STATX_INO) != STATX_INO))
    {
    fail_unsupported:
      emit_error(BOOST_ERROR_NOT_SUPPORTED, p1, p2, ec, "boost::filesystem::equivalent");
      return false;
    }
  }

  struct ::statx s1;
  int e1 = statx(AT_FDCWD, p1.c_str(), AT_NO_AUTOMOUNT, STATX_INO, &s1);
  if (BOOST_LIKELY(e1 == 0))
  {
    if (BOOST_UNLIKELY((s1.stx_mask & STATX_INO) != STATX_INO))
      goto fail_unsupported;
  }
#else
  struct ::stat s2;
  int e2 = ::stat(p2.c_str(), &s2);
  struct ::stat s1;
  int e1 = ::stat(p1.c_str(), &s1);
#endif

  if (BOOST_UNLIKELY(e1 != 0 || e2 != 0))
  {
    // if one is invalid and the other isn't then they aren't equivalent,
    // but if both are invalid then it is an error
    if (e1 != 0 && e2 != 0)
      emit_error(errno, p1, p2, ec, "boost::filesystem::equivalent");
    return false;
  }

  return equivalent_stat(s1, s2);

# else  // Windows

  // Note well: Physical location on external media is part of the
  // equivalence criteria. If there are no open handles, physical location
  // can change due to defragmentation or other relocations. Thus handles
  // must be held open until location information for both paths has
  // been retrieved.

  // p2 is done first, so any error reported is for p1
  handle_wrapper h2(
    create_file_handle(
        p2.c_str(),
        0,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        0,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        0));

  handle_wrapper h1(
    create_file_handle(
        p1.c_str(),
        0,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        0,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        0));

  if (BOOST_UNLIKELY(h1.handle == INVALID_HANDLE_VALUE || h2.handle == INVALID_HANDLE_VALUE))
  {
    // if one is invalid and the other isn't, then they aren't equivalent,
    // but if both are invalid then it is an error
    if (h1.handle == INVALID_HANDLE_VALUE && h2.handle == INVALID_HANDLE_VALUE)
      error(BOOST_ERRNO, p1, p2, ec, "boost::filesystem::equivalent");
    return false;
  }

  // at this point, both handles are known to be valid

  BY_HANDLE_FILE_INFORMATION info1, info2;

  if (error(!::GetFileInformationByHandle(h1.handle, &info1) ? BOOST_ERRNO : 0,
    p1, p2, ec, "boost::filesystem::equivalent"))
    return  false;

  if (error(!::GetFileInformationByHandle(h2.handle, &info2) ? BOOST_ERRNO : 0,
    p1, p2, ec, "boost::filesystem::equivalent"))
    return  false;

  // In theory, volume serial numbers are sufficient to distinguish between
  // devices, but in practice VSN's are sometimes duplicated, so last write
  // time and file size are also checked.
  return
    info1.dwVolumeSerialNumber == info2.dwVolumeSerialNumber
    && info1.nFileIndexHigh == info2.nFileIndexHigh
    && info1.nFileIndexLow == info2.nFileIndexLow
    && info1.nFileSizeHigh == info2.nFileSizeHigh
    && info1.nFileSizeLow == info2.nFileSizeLow
    && info1.ftLastWriteTime.dwLowDateTime
      == info2.ftLastWriteTime.dwLowDateTime
    && info1.ftLastWriteTime.dwHighDateTime
      == info2.ftLastWriteTime.dwHighDateTime;

# endif
}

BOOST_FILESYSTEM_DECL
uintmax_t file_size(const path& p, error_code* ec)
{
  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  struct ::statx path_stat;
  if (BOOST_UNLIKELY(statx(AT_FDCWD, p.c_str(), AT_NO_AUTOMOUNT, STATX_TYPE | STATX_SIZE, &path_stat) < 0))
  {
    emit_error(errno, p, ec, "boost::filesystem::file_size");
    return static_cast<uintmax_t>(-1);
  }

  if (BOOST_UNLIKELY((path_stat.stx_mask & (STATX_TYPE | STATX_SIZE)) != (STATX_TYPE | STATX_SIZE) || !S_ISREG(path_stat.stx_mode)))
  {
    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::file_size");
    return static_cast<uintmax_t>(-1);
  }
#else
  struct ::stat path_stat;
  if (BOOST_UNLIKELY(::stat(p.c_str(), &path_stat) < 0))
  {
    emit_error(errno, p, ec, "boost::filesystem::file_size");
    return static_cast<uintmax_t>(-1);
  }

  if (BOOST_UNLIKELY(!S_ISREG(path_stat.st_mode)))
  {
    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::file_size");
    return static_cast<uintmax_t>(-1);
  }
#endif

  return get_size(path_stat);

#else // defined(BOOST_POSIX_API)

  // assume uintmax_t is 64-bits on all Windows compilers

  WIN32_FILE_ATTRIBUTE_DATA fad;

  if (BOOST_UNLIKELY(!::GetFileAttributesExW(p.c_str(), ::GetFileExInfoStandard, &fad)))
  {
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::file_size");
    return static_cast<uintmax_t>(-1);
  }

  if (BOOST_UNLIKELY((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
  {
    emit_error(ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::file_size");
    return static_cast<uintmax_t>(-1);
  }

  return (static_cast<uintmax_t>(fad.nFileSizeHigh)
    << (sizeof(fad.nFileSizeLow) * 8u)) | fad.nFileSizeLow;

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
uintmax_t hard_link_count(const path& p, system::error_code* ec)
{
  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  struct ::statx path_stat;
  if (BOOST_UNLIKELY(statx(AT_FDCWD, p.c_str(), AT_NO_AUTOMOUNT, STATX_NLINK, &path_stat) < 0))
  {
    emit_error(errno, p, ec, "boost::filesystem::hard_link_count");
    return static_cast<uintmax_t>(-1);
  }

  if (BOOST_UNLIKELY((path_stat.stx_mask & STATX_NLINK) != STATX_NLINK))
  {
    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::hard_link_count");
    return static_cast<uintmax_t>(-1);
  }

  return static_cast<uintmax_t>(path_stat.stx_nlink);
#else
  struct ::stat path_stat;
  if (BOOST_UNLIKELY(::stat(p.c_str(), &path_stat) < 0))
  {
    emit_error(errno, p, ec, "boost::filesystem::hard_link_count");
    return static_cast<uintmax_t>(-1);
  }

  return static_cast<uintmax_t>(path_stat.st_nlink);
#endif

#else // defined(BOOST_POSIX_API)

  handle_wrapper h(
    create_file_handle(p.c_str(), 0,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0));

  if (BOOST_UNLIKELY(h.handle == INVALID_HANDLE_VALUE))
  {
  fail_errno:
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::hard_link_count");
    return static_cast<uintmax_t>(-1);
  }

  // Link count info is only available through GetFileInformationByHandle
  BY_HANDLE_FILE_INFORMATION info;
  if (BOOST_UNLIKELY(!::GetFileInformationByHandle(h.handle, &info)))
    goto fail_errno;

  return static_cast<uintmax_t>(info.nNumberOfLinks);

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
path initial_path(error_code* ec)
{
  static path init_path;
  if (init_path.empty())
    init_path = current_path(ec);
  else if (ec != 0) ec->clear();
  return init_path;
}

BOOST_FILESYSTEM_DECL
bool is_empty(const path& p, system::error_code* ec)
{
  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  struct ::statx path_stat;
  if (BOOST_UNLIKELY(statx(AT_FDCWD, p.c_str(), AT_NO_AUTOMOUNT, STATX_TYPE | STATX_SIZE, &path_stat) < 0))
  {
    emit_error(errno, p, ec, "boost::filesystem::is_empty");
    return false;
  }

  if (BOOST_UNLIKELY((path_stat.stx_mask & STATX_TYPE) != STATX_TYPE))
  {
  fail_unsupported:
    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::is_empty");
    return false;
  }

  if (S_ISDIR(get_mode(path_stat)))
    return is_empty_directory(p, ec);

  if (BOOST_UNLIKELY((path_stat.stx_mask & STATX_SIZE) != STATX_SIZE))
    goto fail_unsupported;

  return get_size(path_stat) == 0u;
#else
  struct ::stat path_stat;
  if (BOOST_UNLIKELY(::stat(p.c_str(), &path_stat) < 0))
  {
    emit_error(errno, p, ec, "boost::filesystem::is_empty");
    return false;
  }

  return S_ISDIR(get_mode(path_stat))
    ? is_empty_directory(p, ec)
    : get_size(path_stat) == 0u;
#endif

#else // defined(BOOST_POSIX_API)

  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (BOOST_UNLIKELY(!::GetFileAttributesExW(p.c_str(), ::GetFileExInfoStandard, &fad)))
  {
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::is_empty");
    return false;
  }

  return
    (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      ? is_empty_directory(p, ec)
      : (!fad.nFileSizeHigh && !fad.nFileSizeLow);

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
std::time_t creation_time(const path& p, system::error_code* ec)
{
  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  struct ::statx stx;
  if (BOOST_UNLIKELY(statx(AT_FDCWD, p.c_str(), AT_NO_AUTOMOUNT, STATX_BTIME, &stx) < 0))
  {
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::creation_time");
    return (std::numeric_limits< std::time_t >::min)();
  }
  if (BOOST_UNLIKELY((stx.stx_mask & STATX_BTIME) != STATX_BTIME))
  {
    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::creation_time");
    return (std::numeric_limits< std::time_t >::min)();
  }
  return stx.stx_btime.tv_sec;
#elif defined(BOOST_FILESYSTEM_STAT_ST_BIRTHTIME) && defined(BOOST_FILESYSTEM_STAT_ST_BIRTHTIMENSEC)
  struct ::stat st;
  if (BOOST_UNLIKELY(::stat(p.c_str(), &st) < 0))
  {
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::creation_time");
    return (std::numeric_limits< std::time_t >::min)();
  }
  return st.BOOST_FILESYSTEM_STAT_ST_BIRTHTIME;
#else
  emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::creation_time");
  return (std::numeric_limits< std::time_t >::min)();
#endif

#else // defined(BOOST_POSIX_API)

  handle_wrapper hw(
    create_file_handle(p.c_str(), 0,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0));

  if (BOOST_UNLIKELY(hw.handle == INVALID_HANDLE_VALUE))
  {
  fail:
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::creation_time");
    return (std::numeric_limits< std::time_t >::min)();
  }

  FILETIME ct;

  if (BOOST_UNLIKELY(!::GetFileTime(hw.handle, &ct, NULL, NULL)))
    goto fail;

  return to_time_t(ct);

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
std::time_t last_write_time(const path& p, system::error_code* ec)
{
  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  struct ::statx stx;
  if (BOOST_UNLIKELY(statx(AT_FDCWD, p.c_str(), AT_NO_AUTOMOUNT, STATX_MTIME, &stx) < 0))
  {
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");
    return (std::numeric_limits< std::time_t >::min)();
  }
  if (BOOST_UNLIKELY((stx.stx_mask & STATX_MTIME) != STATX_MTIME))
  {
    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::last_write_time");
    return (std::numeric_limits< std::time_t >::min)();
  }
  return stx.stx_mtime.tv_sec;
#else
  struct ::stat st;
  if (BOOST_UNLIKELY(::stat(p.c_str(), &st) < 0))
  {
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");
    return (std::numeric_limits< std::time_t >::min)();
  }
  return st.st_mtime;
#endif

#else // defined(BOOST_POSIX_API)

  handle_wrapper hw(
    create_file_handle(p.c_str(), 0,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0));

  if (BOOST_UNLIKELY(hw.handle == INVALID_HANDLE_VALUE))
  {
  fail:
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");
    return (std::numeric_limits< std::time_t >::min)();
  }

  FILETIME lwt;

  if (BOOST_UNLIKELY(!::GetFileTime(hw.handle, NULL, NULL, &lwt)))
    goto fail;

  return to_time_t(lwt);

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
void last_write_time(const path& p, const std::time_t new_time, system::error_code* ec)
{
  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

#if _POSIX_C_SOURCE >= 200809L

  struct timespec times[2] = {};

  // Keep the last access time unchanged
  times[0].tv_nsec = UTIME_OMIT;

  times[1].tv_sec = new_time;

  if (BOOST_UNLIKELY(::utimensat(AT_FDCWD, p.c_str(), times, 0) != 0))
  {
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");
    return;
  }

#else // _POSIX_C_SOURCE >= 200809L

  struct ::stat st;
  if (BOOST_UNLIKELY(::stat(p.c_str(), &st) < 0))
  {
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");
    return;
  }

  ::utimbuf buf;
  buf.actime = st.st_atime; // utime()updates access time too:-(
  buf.modtime = new_time;
  if (BOOST_UNLIKELY(::utime(p.c_str(), &buf) < 0))
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");

#endif // _POSIX_C_SOURCE >= 200809L

#else // defined(BOOST_POSIX_API)

  handle_wrapper hw(
    create_file_handle(p.c_str(), FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0));

  if (BOOST_UNLIKELY(hw.handle == INVALID_HANDLE_VALUE))
  {
  fail:
    emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");
    return;
  }

  FILETIME lwt;
  to_FILETIME(new_time, lwt);

  if (BOOST_UNLIKELY(!::SetFileTime(hw.handle, 0, 0, &lwt)))
    goto fail;

#endif // defined(BOOST_POSIX_API)
}

#ifdef BOOST_POSIX_API
const perms active_bits(all_all | set_uid_on_exe | set_gid_on_exe | sticky_bit);
inline mode_t mode_cast(perms prms) { return prms & active_bits; }
#endif

BOOST_FILESYSTEM_DECL
void permissions(const path& p, perms prms, system::error_code* ec)
{
  BOOST_ASSERT_MSG(!((prms & add_perms) && (prms & remove_perms)),
    "add_perms and remove_perms are mutually exclusive");

  if ((prms & add_perms) && (prms & remove_perms))  // precondition failed
    return;

# if defined(__wasm)
  emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::permissions");
# elif defined(BOOST_POSIX_API)
  error_code local_ec;
  file_status current_status((prms & symlink_perms)
                             ? fs::symlink_status(p, local_ec)
                             : fs::status(p, local_ec));
  if (local_ec)
  {
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error(
        "boost::filesystem::permissions", p, local_ec));
    else
      *ec = local_ec;
    return;
  }

  if (prms & add_perms)
    prms |= current_status.permissions();
  else if (prms & remove_perms)
    prms = current_status.permissions() & ~prms;

  // OS X <10.10, iOS <8.0 and some other platforms don't support fchmodat().
  // Solaris (SunPro and gcc) only support fchmodat() on Solaris 11 and higher,
  // and a runtime check is too much trouble.
  // Linux does not support permissions on symbolic links and has no plans to
  // support them in the future.  The chmod() code is thus more practical,
  // rather than always hitting ENOTSUP when sending in AT_SYMLINK_NO_FOLLOW.
  //  - See the 3rd paragraph of
  // "Symbolic link ownership, permissions, and timestamps" at:
  //   "http://man7.org/linux/man-pages/man7/symlink.7.html"
  //  - See the fchmodat() Linux man page:
  //   "http://man7.org/linux/man-pages/man2/fchmodat.2.html"
# if defined(AT_FDCWD) && defined(AT_SYMLINK_NOFOLLOW) \
    && !(defined(__SUNPRO_CC) || defined(__sun) || defined(sun)) \
    && !(defined(linux) || defined(__linux) || defined(__linux__)) \
    && !(defined(__MAC_OS_X_VERSION_MIN_REQUIRED) \
         && __MAC_OS_X_VERSION_MIN_REQUIRED < 101000) \
    && !(defined(__IPHONE_OS_VERSION_MIN_REQUIRED) \
         && __IPHONE_OS_VERSION_MIN_REQUIRED < 80000) \
    && !(defined(__QNX__) && (_NTO_VERSION <= 700))
  if (::fchmodat(AT_FDCWD, p.c_str(), mode_cast(prms),
       !(prms & symlink_perms) ? 0 : AT_SYMLINK_NOFOLLOW))
# else  // fallback if fchmodat() not supported
  if (::chmod(p.c_str(), mode_cast(prms)))
# endif
  {
    const int err = errno;
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error(
        "boost::filesystem::permissions", p,
        error_code(err, system::generic_category())));
    else
      ec->assign(err, system::generic_category());
  }

# else  // Windows

  // if not going to alter FILE_ATTRIBUTE_READONLY, just return
  if (!(!((prms & (add_perms | remove_perms)))
    || (prms & (owner_write|group_write|others_write))))
    return;

  DWORD attr = ::GetFileAttributesW(p.c_str());

  if (error(attr == 0 ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::permissions"))
    return;

  if (prms & add_perms)
    attr &= ~FILE_ATTRIBUTE_READONLY;
  else if (prms & remove_perms)
    attr |= FILE_ATTRIBUTE_READONLY;
  else if (prms & (owner_write|group_write|others_write))
    attr &= ~FILE_ATTRIBUTE_READONLY;
  else
    attr |= FILE_ATTRIBUTE_READONLY;

  error(::SetFileAttributesW(p.c_str(), attr) == 0 ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::permissions");
# endif
}

BOOST_FILESYSTEM_DECL
path read_symlink(const path& p, system::error_code* ec)
{
  path symlink_path;

# ifdef BOOST_POSIX_API
  const char* const path_str = p.c_str();
  char small_buf[1024];
  ssize_t result = ::readlink(path_str, small_buf, sizeof(small_buf));
  if (BOOST_UNLIKELY(result < 0))
  {
  fail:
    const int err = errno;
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::read_symlink",
        p, error_code(err, system_category())));
    else
      ec->assign(err, system_category());
  }
  else if (BOOST_LIKELY(static_cast< std::size_t >(result) < sizeof(small_buf)))
  {
    symlink_path.assign(small_buf, small_buf + result);
    if (ec != 0)
      ec->clear();
  }
  else
  {
    for (std::size_t path_max = sizeof(small_buf) * 2u;; path_max *= 2u) // loop 'til buffer large enough
    {
      if (BOOST_UNLIKELY(path_max > absolute_path_max))
      {
        if (ec == 0)
          BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::read_symlink",
            p, error_code(ENAMETOOLONG, system_category())));
        else
          ec->assign(ENAMETOOLONG, system_category());
        break;
      }

      boost::scoped_array<char> buf(new char[path_max]);
      result = ::readlink(path_str, buf.get(), path_max);
      if (BOOST_UNLIKELY(result < 0))
      {
        goto fail;
      }
      else if (BOOST_LIKELY(static_cast< std::size_t >(result) < path_max))
      {
        symlink_path.assign(buf.get(), buf.get() + result);
        if (ec != 0) ec->clear();
        break;
      }
    }
  }

# else

  handle_wrapper h(
    create_file_handle(p.c_str(), 0,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, 0));

  if (error(h.handle == INVALID_HANDLE_VALUE ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::read_symlink"))
      return symlink_path;

  boost::scoped_ptr<reparse_data_buffer> buf(new reparse_data_buffer);
  DWORD sz = 0u;
  if (!error(::DeviceIoControl(h.handle, FSCTL_GET_REPARSE_POINT,
        0, 0, buf.get(), sizeof(*buf), &sz, 0) == 0 ? BOOST_ERRNO : 0, p, ec,
        "boost::filesystem::read_symlink" ))
  {
    const wchar_t* buffer;
    std::size_t offset, len;
    switch (buf->rdb.ReparseTag)
    {
    case IO_REPARSE_TAG_MOUNT_POINT:
      buffer = buf->rdb.MountPointReparseBuffer.PathBuffer;
      offset = buf->rdb.MountPointReparseBuffer.PrintNameOffset;
      len = buf->rdb.MountPointReparseBuffer.PrintNameLength;
      break;
    case IO_REPARSE_TAG_SYMLINK:
      buffer = buf->rdb.SymbolicLinkReparseBuffer.PathBuffer;
      offset = buf->rdb.SymbolicLinkReparseBuffer.PrintNameOffset;
      len = buf->rdb.SymbolicLinkReparseBuffer.PrintNameLength;
      // Note: iff info.rdb.SymbolicLinkReparseBuffer.Flags & SYMLINK_FLAG_RELATIVE
      //       -> resulting path is relative to the source
      break;
    default:
      emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "Unknown ReparseTag in boost::filesystem::read_symlink");
      return symlink_path;
    }
    symlink_path.assign(
      buffer + offset / sizeof(wchar_t),
      buffer + (offset + len) / sizeof(wchar_t));
  }
# endif
  return symlink_path;
}

BOOST_FILESYSTEM_DECL
path relative(const path& p, const path& base, error_code* ec)
{
  error_code tmp_ec;
  path wc_base(weakly_canonical(base, &tmp_ec));
  if (error(tmp_ec.value(), base, ec, "boost::filesystem::relative"))
    return path();
  path wc_p(weakly_canonical(p, &tmp_ec));
  if (error(tmp_ec.value(), base, ec, "boost::filesystem::relative"))
    return path();
  return wc_p.lexically_relative(wc_base);
}

BOOST_FILESYSTEM_DECL
bool remove(const path& p, error_code* ec)
{
  error_code tmp_ec;
  file_type type = query_file_type(p, &tmp_ec);
  if (error(type == status_error ? tmp_ec.value() : 0, p, ec,
      "boost::filesystem::remove"))
    return false;

  // Since POSIX remove() is specified to work with either files or directories, in a
  // perfect world it could just be called. But some important real-world operating
  // systems (Windows, Mac OS X, for example) don't implement the POSIX spec. So
  // remove_file_or_directory() is always called to keep it simple.
  return remove_file_or_directory(p, type, ec);
}

BOOST_FILESYSTEM_DECL
uintmax_t remove_all(const path& p, error_code* ec)
{
  error_code tmp_ec;
  file_type type = query_file_type(p, &tmp_ec);
  if (error(type == status_error ? tmp_ec.value() : 0, p, ec,
    "boost::filesystem::remove_all"))
    return 0;

  return (type != status_error && type != file_not_found) // exists
    ? remove_all_aux(p, type, ec)
    : 0;
}

BOOST_FILESYSTEM_DECL
void rename(const path& old_p, const path& new_p, error_code* ec)
{
  error(!BOOST_MOVE_FILE(old_p.c_str(), new_p.c_str()) ? BOOST_ERRNO : 0, old_p, new_p,
    ec, "boost::filesystem::rename");
}

BOOST_FILESYSTEM_DECL
void resize_file(const path& p, uintmax_t size, system::error_code* ec)
{
# if defined(BOOST_POSIX_API)
  if (BOOST_UNLIKELY(size > static_cast< uintmax_t >((std::numeric_limits< off_t >::max)()))) {
    emit_error(system::errc::file_too_large, p, ec, "boost::filesystem::resize_file");
    return;
  }
# endif
  error(!BOOST_RESIZE_FILE(p.c_str(), size) ? BOOST_ERRNO : 0, p, ec,
    "boost::filesystem::resize_file");
}

BOOST_FILESYSTEM_DECL
space_info space(const path& p, error_code* ec)
{
  space_info info;
  // Initialize members to -1, as required by C++20 [fs.op.space]/1 in case of error
  info.capacity = static_cast<uintmax_t>(-1);
  info.free = static_cast<uintmax_t>(-1);
  info.available = static_cast<uintmax_t>(-1);

  if (ec)
    ec->clear();

# if defined(__wasm)

  emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::space");

# elif defined(BOOST_POSIX_API)

  struct BOOST_STATVFS vfs;
  if (!error(::BOOST_STATVFS(p.c_str(), &vfs) ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::space"))
  {
    info.capacity
      = static_cast<uintmax_t>(vfs.f_blocks) * BOOST_STATVFS_F_FRSIZE;
    info.free
      = static_cast<uintmax_t>(vfs.f_bfree) * BOOST_STATVFS_F_FRSIZE;
    info.available
      = static_cast<uintmax_t>(vfs.f_bavail) * BOOST_STATVFS_F_FRSIZE;
  }

# else

  // GetDiskFreeSpaceExW requires a directory path, which is unlike statvfs, which accepts any file.
  // To work around this, test if the path refers to a directory and use the parent directory if not.
  error_code local_ec;
  file_status status = detail::status(p, &local_ec);
  if (status.type() == fs::status_error || status.type() == fs::file_not_found)
  {
  fail_local_ec:
    if (!ec)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::space", p, local_ec));
    *ec = local_ec;
    return info;
  }

  path dir_path = p;
  if (!is_directory(status))
  {
    path cur_path = detail::current_path(ec);
    if (ec && *ec)
      return info;

    status = detail::symlink_status(p, &local_ec);
    if (status.type() == fs::status_error)
      goto fail_local_ec;
    if (is_symlink(status))
    {
      // We need to resolve the symlink so that we report the space for the symlink target
      dir_path = detail::canonical(p, cur_path, ec);
      if (ec && *ec)
        return info;
    }

    dir_path = dir_path.parent_path();
    if (dir_path.empty())
    {
      // The original path was just a filename, which is a relative path wrt. current directory
      dir_path = cur_path;
    }
  }

  // For UNC names, the path must also include a trailing slash.
  path::string_type str = dir_path.native();
  if (str.size() >= 2u && detail::is_directory_separator(str[0]) && detail::is_directory_separator(str[1]) && !detail::is_directory_separator(*(str.end() - 1)))
    str.push_back(path::preferred_separator);

  ULARGE_INTEGER avail, total, free;
  if (!error(::GetDiskFreeSpaceExW(str.c_str(), &avail, &total, &free) == 0,
     p, ec, "boost::filesystem::space"))
  {
    info.capacity = static_cast<uintmax_t>(total.QuadPart);
    info.free = static_cast<uintmax_t>(free.QuadPart);
    info.available = static_cast<uintmax_t>(avail.QuadPart);
  }

# endif

  return info;
}

BOOST_FILESYSTEM_DECL
file_status status(const path& p, error_code* ec)
{
  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  struct ::statx path_stat;
  int err = statx(AT_FDCWD, p.c_str(), AT_NO_AUTOMOUNT, STATX_TYPE | STATX_MODE, &path_stat);
#else
  struct ::stat path_stat;
  int err = ::stat(p.c_str(), &path_stat);
#endif

  if (err != 0)
  {
    err = errno;
    if (ec != 0)                            // always report errno, even though some
      ec->assign(err, system_category());   // errno values are not status_errors

    if (not_found_error(err))
    {
      return fs::file_status(fs::file_not_found, fs::no_perms);
    }
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::status",
        p, error_code(err, system_category())));
    return fs::file_status(fs::status_error);
  }

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  if (BOOST_UNLIKELY((path_stat.stx_mask & (STATX_TYPE | STATX_MODE)) != (STATX_TYPE | STATX_MODE)))
  {
    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::status");
    return fs::file_status(fs::status_error);
  }
#endif

  const mode_t mode = get_mode(path_stat);
  if (S_ISDIR(mode))
    return fs::file_status(fs::directory_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISREG(mode))
    return fs::file_status(fs::regular_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISBLK(mode))
    return fs::file_status(fs::block_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISCHR(mode))
    return fs::file_status(fs::character_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISFIFO(mode))
    return fs::file_status(fs::fifo_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISSOCK(mode))
    return fs::file_status(fs::socket_file,
      static_cast<perms>(mode) & fs::perms_mask);
  return fs::file_status(fs::type_unknown);

#else // defined(BOOST_POSIX_API)

  DWORD attr(::GetFileAttributesW(p.c_str()));
  if (attr == 0xFFFFFFFF)
  {
    return process_status_failure(p, ec);
  }

  perms permissions = make_permissions(p, attr);

  //  reparse point handling;
  //    since GetFileAttributesW does not resolve symlinks, try to open a file
  //    handle to discover if the file exists
  if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
  {
    if (!is_reparse_point_a_symlink(p))
    {
      return file_status(reparse_file, permissions);
    }

    // try to resolve symlink
    handle_wrapper h(
      create_file_handle(
          p.c_str(),
          0,  // dwDesiredAccess; attributes only
          FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
          0,  // lpSecurityAttributes
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS,
          0)); // hTemplateFile

    if (h.handle == INVALID_HANDLE_VALUE)
    {
      return process_status_failure(p, ec);
    }

    // take attributes of target
    BY_HANDLE_FILE_INFORMATION info;
    if (!::GetFileInformationByHandle(h.handle, &info))
    {
      return process_status_failure(p, ec);
    }

    attr = info.dwFileAttributes;
  }

  return (attr & FILE_ATTRIBUTE_DIRECTORY)
    ? file_status(directory_file, permissions)
    : file_status(regular_file, permissions);

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
file_status symlink_status(const path& p, error_code* ec)
{
  if (ec)
    ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  struct ::statx path_stat;
  int err = statx(AT_FDCWD, p.c_str(), AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_TYPE | STATX_MODE, &path_stat);
#else
  struct ::stat path_stat;
  int err = ::lstat(p.c_str(), &path_stat);
#endif

  if (err != 0)
  {
    err = errno;
    if (ec != 0)                            // always report errno, even though some
      ec->assign(err, system_category());   // errno values are not status_errors

    if (not_found_error(err)) // these are not errors
    {
      return fs::file_status(fs::file_not_found, fs::no_perms);
    }
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::symlink_status",
        p, error_code(err, system_category())));
    return fs::file_status(fs::status_error);
  }

#if defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
  if (BOOST_UNLIKELY((path_stat.stx_mask & (STATX_TYPE | STATX_MODE)) != (STATX_TYPE | STATX_MODE)))
  {
    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::symlink_status");
    return fs::file_status(fs::status_error);
  }
#endif

  const mode_t mode = get_mode(path_stat);
  if (S_ISREG(mode))
    return fs::file_status(fs::regular_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISDIR(mode))
    return fs::file_status(fs::directory_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISLNK(mode))
    return fs::file_status(fs::symlink_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISBLK(mode))
    return fs::file_status(fs::block_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISCHR(mode))
    return fs::file_status(fs::character_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISFIFO(mode))
    return fs::file_status(fs::fifo_file,
      static_cast<perms>(mode) & fs::perms_mask);
  if (S_ISSOCK(mode))
    return fs::file_status(fs::socket_file,
      static_cast<perms>(mode) & fs::perms_mask);
  return fs::file_status(fs::type_unknown);

#else // defined(BOOST_POSIX_API)

  DWORD attr(::GetFileAttributesW(p.c_str()));
  if (attr == 0xFFFFFFFF)
  {
    return process_status_failure(p, ec);
  }

  perms permissions = make_permissions(p, attr);

  if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
    return is_reparse_point_a_symlink(p)
           ? file_status(symlink_file, permissions)
           : file_status(reparse_file, permissions);

  return (attr & FILE_ATTRIBUTE_DIRECTORY)
    ? file_status(directory_file, permissions)
    : file_status(regular_file, permissions);

#endif // defined(BOOST_POSIX_API)
}

 // contributed by Jeff Flinn
BOOST_FILESYSTEM_DECL
path temp_directory_path(system::error_code* ec)
{
  if (ec)
    ec->clear();

# ifdef BOOST_POSIX_API
  const char* val = 0;

  (val = std::getenv("TMPDIR" )) ||
  (val = std::getenv("TMP"    )) ||
  (val = std::getenv("TEMP"   )) ||
  (val = std::getenv("TEMPDIR"));

#   ifdef __ANDROID__
  const char* default_tmp = "/data/local/tmp";
#   else
  const char* default_tmp = "/tmp";
#   endif
  path p((val != NULL) ? val : default_tmp);

  if (BOOST_UNLIKELY(p.empty()))
  {
  fail_not_dir:
    error(ENOTDIR, p, ec, "boost::filesystem::temp_directory_path");
    return p;
  }

  file_status status = detail::status(p, ec);
  if (BOOST_UNLIKELY(ec && *ec))
    return path();
  if (BOOST_UNLIKELY(!is_directory(status)))
    goto fail_not_dir;

  return p;

# else   // Windows
# if !defined(UNDER_CE)

  const wchar_t* tmp_env = L"TMP";
  const wchar_t* temp_env = L"TEMP";
  const wchar_t* localappdata_env = L"LOCALAPPDATA";
  const wchar_t* userprofile_env = L"USERPROFILE";
  const wchar_t* env_list[] = { tmp_env, temp_env, localappdata_env, userprofile_env };

  path p;
  for (unsigned int i = 0; i < sizeof(env_list) / sizeof(*env_list); ++i)
  {
    std::wstring env = wgetenv(env_list[i]);
    if (!env.empty())
    {
      p = env;
      if (i >= 2)
        p /= L"Temp";
      error_code lcl_ec;
      if (exists(p, lcl_ec) && !lcl_ec && is_directory(p, lcl_ec) && !lcl_ec)
        break;
      p.clear();
    }
  }

  if (p.empty())
  {
    // use a separate buffer since in C++03 a string is not required to be contiguous
    const UINT size = ::GetWindowsDirectoryW(NULL, 0);
    if (BOOST_UNLIKELY(size == 0))
    {
    getwindir_error:
      int errval = ::GetLastError();
      error(errval, ec, "boost::filesystem::temp_directory_path");
      return path();
    }

    boost::scoped_array<wchar_t> buf(new wchar_t[size]);
    if (BOOST_UNLIKELY(::GetWindowsDirectoryW(buf.get(), size) == 0))
      goto getwindir_error;

    p = buf.get();  // do not depend on initial buf size, see ticket #10388
    p /= L"Temp";
  }

  return p;

# else // Windows CE

  // Windows CE has no environment variables, so the same code as used for
  // regular Windows, above, doesn't work.

  DWORD size = ::GetTempPathW(0, NULL);
  if (size == 0u)
  {
  fail:
    int errval = ::GetLastError();
    error(errval, ec, "boost::filesystem::temp_directory_path");
    return path();
  }

  boost::scoped_array<wchar_t> buf(new wchar_t[size]);
  if (::GetTempPathW(size, buf.get()) == 0)
    goto fail;

  path p(buf.get());
  p.remove_trailing_separator();

  file_status status = detail::status(p, ec);
  if (ec && *ec)
    return path();
  if (!is_directory(status))
  {
    error(ERROR_PATH_NOT_FOUND, p, ec, "boost::filesystem::temp_directory_path");
    return path();
  }

  return p;

# endif // !defined(UNDER_CE)
# endif
}

BOOST_FILESYSTEM_DECL
path system_complete(const path& p, system::error_code* ec)
{
# ifdef BOOST_POSIX_API
  return (p.empty() || p.is_absolute())
    ? p : current_path() / p;

# else
  if (p.empty())
  {
    if (ec != 0) ec->clear();
    return p;
  }

  BOOST_CONSTEXPR_OR_CONST std::size_t buf_size = 128;
  wchar_t buf[buf_size];
  wchar_t* pfn;
  std::size_t len = get_full_path_name(p, buf_size, buf, &pfn);

  if (error(len == 0 ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::system_complete"))
    return path();

  if (len < buf_size)// len does not include null termination character
    return path(&buf[0]);

  boost::scoped_array<wchar_t> big_buf(new wchar_t[len]);

  return error(get_full_path_name(p, len , big_buf.get(), &pfn)== 0 ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::system_complete")
    ? path()
    : path(big_buf.get());
# endif
}

BOOST_FILESYSTEM_DECL
path weakly_canonical(const path& p, system::error_code* ec)
{
  path head(p);
  path tail;
  system::error_code tmp_ec;
  path::iterator itr = p.end();

  for (; !head.empty(); --itr)
  {
    file_status head_status = status(head, tmp_ec);
    if (error(head_status.type() == fs::status_error,
      head, ec, "boost::filesystem::weakly_canonical"))
      return path();
    if (head_status.type() != fs::file_not_found)
      break;
    head.remove_filename();
  }

  bool tail_has_dots = false;
  for (; itr != p.end(); ++itr)
  {
    tail /= *itr;
    // for a later optimization, track if any dot or dot-dot elements are present
    if (itr->native().size() <= 2
      && itr->native()[0] == dot
      && (itr->native().size() == 1 || itr->native()[1] == dot))
      tail_has_dots = true;
  }

  if (head.empty())
    return p.lexically_normal();
  head = canonical(head, tmp_ec);
  if (error(tmp_ec.value(), head, ec, "boost::filesystem::weakly_canonical"))
    return path();
  return tail.empty()
    ? head
    : (tail_has_dots  // optimization: only normalize if tail had dot or dot-dot element
        ? (head/tail).lexically_normal()
        : head/tail);
}

} // namespace detail
} // namespace filesystem
} // namespace boost
