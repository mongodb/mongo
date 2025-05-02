//  operations.cpp  --------------------------------------------------------------------//

//  Copyright 2002-2009, 2014 Beman Dawes
//  Copyright 2001 Dietmar Kuehl
//  Copyright 2018-2024 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#include "platform_config.hpp"

#include <boost/predef/os/bsd/open.h>
#include <boost/filesystem/config.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/system/error_code.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/core/bit.hpp>
#include <boost/cstdint.hpp>
#include <boost/assert.hpp>
#include <new> // std::bad_alloc, std::nothrow
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <cstddef>
#include <cstdlib> // for malloc, free
#include <cstring>
#include <cerrno>
#include <stdio.h> // for rename

// Default to POSIX under Emscripten
// If BOOST_FILESYSTEM_EMSCRIPTEN_USE_WASI is set, use WASI instead
#if defined(__wasm) && (!defined(__EMSCRIPTEN__) || defined(BOOST_FILESYSTEM_EMSCRIPTEN_USE_WASI))
#define BOOST_FILESYSTEM_USE_WASI
#endif

#ifdef BOOST_POSIX_API

#include <sys/types.h>
#include <sys/stat.h>

#if defined(BOOST_FILESYSTEM_USE_WASI)
// WASI does not have statfs or statvfs.
#elif !defined(__APPLE__) && \
    (!defined(__OpenBSD__) || BOOST_OS_BSD_OPEN >= BOOST_VERSION_NUMBER(4, 4, 0)) && \
    !defined(__ANDROID__) && \
    !defined(__VXWORKS__)
#include <sys/statvfs.h>
#define BOOST_STATVFS statvfs
#define BOOST_STATVFS_F_FRSIZE vfs.f_frsize
#else
#ifdef __OpenBSD__
#include <sys/param.h>
#elif defined(__ANDROID__)
#include <sys/vfs.h>
#endif
#if !defined(__VXWORKS__)
#include <sys/mount.h>
#endif
#define BOOST_STATVFS statfs
#define BOOST_STATVFS_F_FRSIZE static_cast< uintmax_t >(vfs.f_bsize)
#endif // BOOST_STATVFS definition

#include <unistd.h>
#include <fcntl.h>
#if !defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
#include <utime.h>
#endif
#include <limits.h>

#if defined(linux) || defined(__linux) || defined(__linux__)

#include <sys/vfs.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#if !defined(BOOST_FILESYSTEM_DISABLE_SENDFILE)
#include <sys/sendfile.h>
#define BOOST_FILESYSTEM_USE_SENDFILE
#endif // !defined(BOOST_FILESYSTEM_DISABLE_SENDFILE)
#if !defined(BOOST_FILESYSTEM_DISABLE_COPY_FILE_RANGE) && defined(__NR_copy_file_range)
#define BOOST_FILESYSTEM_USE_COPY_FILE_RANGE
#endif // !defined(BOOST_FILESYSTEM_DISABLE_COPY_FILE_RANGE) && defined(__NR_copy_file_range)
#if !defined(BOOST_FILESYSTEM_DISABLE_STATX) && (defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL))
#if !defined(BOOST_FILESYSTEM_HAS_STATX) && defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
#include <linux/stat.h>
#endif
#define BOOST_FILESYSTEM_USE_STATX
#endif // !defined(BOOST_FILESYSTEM_DISABLE_STATX) && (defined(BOOST_FILESYSTEM_HAS_STATX) || defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL))

#if defined(__has_include)
#if __has_include(<linux/magic.h>)
// This header was introduced in Linux kernel 2.6.19
#include <linux/magic.h>
#endif
#endif

// Some filesystem type magic constants are not defined in older kernel headers
#ifndef PROC_SUPER_MAGIC
#define PROC_SUPER_MAGIC 0x9fa0
#endif
#ifndef SYSFS_MAGIC
#define SYSFS_MAGIC 0x62656572
#endif
#ifndef TRACEFS_MAGIC
#define TRACEFS_MAGIC 0x74726163
#endif
#ifndef DEBUGFS_MAGIC
#define DEBUGFS_MAGIC 0x64626720
#endif

#endif // defined(linux) || defined(__linux) || defined(__linux__)

#include <boost/scope/unique_fd.hpp>

#if defined(POSIX_FADV_SEQUENTIAL) && (!defined(__ANDROID__) || __ANDROID_API__ >= 21)
#define BOOST_FILESYSTEM_HAS_POSIX_FADVISE
#endif

#if defined(BOOST_FILESYSTEM_HAS_STAT_ST_MTIM)
#define BOOST_FILESYSTEM_STAT_ST_MTIMENSEC st_mtim.tv_nsec
#elif defined(BOOST_FILESYSTEM_HAS_STAT_ST_MTIMESPEC)
#define BOOST_FILESYSTEM_STAT_ST_MTIMENSEC st_mtimespec.tv_nsec
#elif defined(BOOST_FILESYSTEM_HAS_STAT_ST_MTIMENSEC)
#define BOOST_FILESYSTEM_STAT_ST_MTIMENSEC st_mtimensec
#endif

#if defined(BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIM)
#define BOOST_FILESYSTEM_STAT_ST_BIRTHTIME st_birthtim.tv_sec
#define BOOST_FILESYSTEM_STAT_ST_BIRTHTIMENSEC st_birthtim.tv_nsec
#elif defined(BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIMESPEC)
#define BOOST_FILESYSTEM_STAT_ST_BIRTHTIME st_birthtimespec.tv_sec
#define BOOST_FILESYSTEM_STAT_ST_BIRTHTIMENSEC st_birthtimespec.tv_nsec
#elif defined(BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIMENSEC)
#define BOOST_FILESYSTEM_STAT_ST_BIRTHTIME st_birthtime
#define BOOST_FILESYSTEM_STAT_ST_BIRTHTIMENSEC st_birthtimensec
#endif

#include "posix_tools.hpp"

#else // BOOST_WINDOWS_API

#include <boost/winapi/dll.hpp> // get_proc_address, GetModuleHandleW
#include <cwchar>
#include <io.h>
#include <windows.h>
#include <winnt.h>
#if defined(__BORLANDC__) || defined(__MWERKS__)
#if defined(BOOST_BORLANDC)
using std::time_t;
#endif
#include <utime.h>
#else
#include <sys/utime.h>
#endif

#include "windows_tools.hpp"

#endif // BOOST_WINDOWS_API

#include "atomic_tools.hpp"
#include "error_handling.hpp"
#include "private_config.hpp"

#include <boost/filesystem/detail/header.hpp> // must be the last #include

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
#define BOOST_FILESYSTEM_NO_O_CLOEXEC
#endif

#if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
#define BOOST_FILESYSTEM_HAS_FDATASYNC
#endif

#else // defined(BOOST_POSIX_API)

#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 * 1024)
#endif

#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT 0x900a8
#endif

#ifndef SYMLINK_FLAG_RELATIVE
#define SYMLINK_FLAG_RELATIVE 1
#endif

// Fallback for MinGW/Cygwin
#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#endif

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

#endif // defined(BOOST_POSIX_API)

//  POSIX/Windows macros  ----------------------------------------------------//

//  Portions of the POSIX and Windows API's are very similar, except for name,
//  order of arguments, and meaning of zero/non-zero returns. The macros below
//  abstract away those differences. They follow Windows naming and order of
//  arguments, and return true to indicate no error occurred. [POSIX naming,
//  order of arguments, and meaning of return were followed initially, but
//  found to be less clear and cause more coding errors.]

#if defined(BOOST_POSIX_API)

#define BOOST_SET_CURRENT_DIRECTORY(P) (::chdir(P) == 0)
#define BOOST_MOVE_FILE(OLD, NEW) (::rename(OLD, NEW) == 0)
#define BOOST_RESIZE_FILE(P, SZ) (::truncate(P, SZ) == 0)

#else // BOOST_WINDOWS_API

#define BOOST_SET_CURRENT_DIRECTORY(P) (::SetCurrentDirectoryW(P) != 0)
#define BOOST_MOVE_FILE(OLD, NEW) (::MoveFileExW(OLD, NEW, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0)
#define BOOST_RESIZE_FILE(P, SZ) (resize_file_impl(P, SZ) != 0)

#endif

namespace boost {
namespace filesystem {
namespace detail {

#if defined(linux) || defined(__linux) || defined(__linux__)
//! Initializes fill_random implementation pointer. Implemented in unique_path.cpp.
void init_fill_random_impl(unsigned int major_ver, unsigned int minor_ver, unsigned int patch_ver);
#endif // defined(linux) || defined(__linux) || defined(__linux__)

#if defined(BOOST_WINDOWS_API)
//! Initializes directory iterator implementation. Implemented in directory.cpp.
void init_directory_iterator_impl() noexcept;
#endif // defined(BOOST_WINDOWS_API)

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                        helpers (all operating systems)                               //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace {

// The number of retries remove_all should make if it detects that the directory it is about to enter has been replaced with a symlink or a regular file
BOOST_CONSTEXPR_OR_CONST unsigned int remove_all_directory_replaced_retry_count = 5u;

// Size of a small buffer for a path that can be placed on stack, in character code units
BOOST_CONSTEXPR_OR_CONST std::size_t small_path_size = 1024u;

#if defined(BOOST_POSIX_API)

// Absolute maximum path length, in character code units, that we're willing to accept from various system calls.
// This value is arbitrary, it is supposed to be a hard limit to avoid memory exhaustion
// in some of the algorithms below in case of some corrupted or maliciously broken filesystem.
// A few examples of path size limits:
// - Windows: 32767 UTF-16 code units or 260 bytes for legacy multibyte APIs.
// - Linux: 4096 bytes
// - IRIX, HP-UX, Mac OS, QNX, FreeBSD, OpenBSD: 1024 bytes
// - GNU/Hurd: no hard limit
BOOST_CONSTEXPR_OR_CONST std::size_t absolute_path_max = 32u * 1024u;

#endif // defined(BOOST_POSIX_API)

// Maximum number of resolved symlinks before we register a loop
BOOST_CONSTEXPR_OR_CONST unsigned int symloop_max =
#if defined(SYMLOOP_MAX)
    SYMLOOP_MAX < 40 ? 40 : SYMLOOP_MAX
#else
    40
#endif
;

//  general helpers  -----------------------------------------------------------------//

#ifdef BOOST_POSIX_API

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            POSIX-specific helpers                                    //
//                                                                                      //
//--------------------------------------------------------------------------------------//

inline bool not_found_error(int errval) noexcept
{
    return errval == ENOENT || errval == ENOTDIR;
}

/*!
 * Closes a file descriptor and returns the result, similar to close(2). Unlike close(2), guarantees that the file descriptor is closed even if EINTR error happens.
 *
 * Some systems don't close the file descriptor in case if the thread is interrupted by a signal and close(2) returns EINTR.
 * Other (most) systems do close the file descriptor even when when close(2) returns EINTR, and attempting to close it
 * again could close a different file descriptor that was opened by a different thread. This function hides this difference in behavior.
 *
 * Future POSIX standards will likely fix this by introducing posix_close (see https://www.austingroupbugs.net/view.php?id=529)
 * and prohibiting returning EINTR from close(2), but we still have to support older systems where this new behavior is not available and close(2)
 * behaves differently between systems.
 */
inline int close_fd(int fd)
{
#if defined(hpux) || defined(_hpux) || defined(__hpux)
    int res;
    while (true)
    {
        res = ::close(fd);
        if (BOOST_UNLIKELY(res < 0))
        {
            int err = errno;
            if (err == EINTR)
                continue;
        }

        break;
    }

    return res;
#else
    return ::close(fd);
#endif
}

#if defined(BOOST_FILESYSTEM_HAS_STATX)

//! A wrapper for statx libc function. Disable MSAN since at least on clang 10 it doesn't
//! know which fields of struct statx are initialized by the syscall and misdetects errors.
BOOST_FILESYSTEM_NO_SANITIZE_MEMORY
BOOST_FORCEINLINE int invoke_statx(int dirfd, const char* path, int flags, unsigned int mask, struct ::statx* stx)
{
    return ::statx(dirfd, path, flags, mask, stx);
}

#elif defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)

//! statx emulation through fstatat
int statx_fstatat(int dirfd, const char* path, int flags, unsigned int mask, struct ::statx* stx)
{
    struct ::stat st;
    flags &= AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW;
    int res = ::fstatat(dirfd, path, &st, flags);
    if (BOOST_LIKELY(res == 0))
    {
        std::memset(stx, 0, sizeof(*stx));
        stx->stx_mask = STATX_BASIC_STATS;
        stx->stx_blksize = st.st_blksize;
        stx->stx_nlink = st.st_nlink;
        stx->stx_uid = st.st_uid;
        stx->stx_gid = st.st_gid;
        stx->stx_mode = st.st_mode;
        stx->stx_ino = st.st_ino;
        stx->stx_size = st.st_size;
        stx->stx_blocks = st.st_blocks;
        stx->stx_atime.tv_sec = st.st_atim.tv_sec;
        stx->stx_atime.tv_nsec = st.st_atim.tv_nsec;
        stx->stx_ctime.tv_sec = st.st_ctim.tv_sec;
        stx->stx_ctime.tv_nsec = st.st_ctim.tv_nsec;
        stx->stx_mtime.tv_sec = st.st_mtim.tv_sec;
        stx->stx_mtime.tv_nsec = st.st_mtim.tv_nsec;
        stx->stx_rdev_major = major(st.st_rdev);
        stx->stx_rdev_minor = minor(st.st_rdev);
        stx->stx_dev_major = major(st.st_dev);
        stx->stx_dev_minor = minor(st.st_dev);
    }

    return res;
}

typedef int statx_t(int dirfd, const char* path, int flags, unsigned int mask, struct ::statx* stx);

//! Pointer to the actual implementation of the statx implementation
statx_t* statx_ptr = &statx_fstatat;

inline int invoke_statx(int dirfd, const char* path, int flags, unsigned int mask, struct ::statx* stx) noexcept
{
    return filesystem::detail::atomic_load_relaxed(statx_ptr)(dirfd, path, flags, mask, stx);
}

//! A wrapper for the statx syscall. Disable MSAN since at least on clang 10 it doesn't
//! know which fields of struct statx are initialized by the syscall and misdetects errors.
BOOST_FILESYSTEM_NO_SANITIZE_MEMORY
int statx_syscall(int dirfd, const char* path, int flags, unsigned int mask, struct ::statx* stx)
{
    int res = ::syscall(__NR_statx, dirfd, path, flags, mask, stx);
    if (res < 0)
    {
        const int err = errno;
        if (BOOST_UNLIKELY(err == ENOSYS))
        {
            filesystem::detail::atomic_store_relaxed(statx_ptr, &statx_fstatat);
            return statx_fstatat(dirfd, path, flags, mask, stx);
        }
    }

    return res;
}

#endif // defined(BOOST_FILESYSTEM_HAS_STATX)

#if defined(linux) || defined(__linux) || defined(__linux__)

//! Initializes statx implementation pointer
inline void init_statx_impl(unsigned int major_ver, unsigned int minor_ver, unsigned int patch_ver)
{
#if !defined(BOOST_FILESYSTEM_HAS_STATX) && defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
    statx_t* stx = &statx_fstatat;
    if (major_ver > 4u || (major_ver == 4u && minor_ver >= 11u))
        stx = &statx_syscall;

    filesystem::detail::atomic_store_relaxed(statx_ptr, stx);
#endif // !defined(BOOST_FILESYSTEM_HAS_STATX) && defined(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
}

#endif // defined(linux) || defined(__linux) || defined(__linux__)

#if defined(BOOST_FILESYSTEM_USE_STATX)

//! Returns \c true if the two \c statx structures refer to the same file
inline bool equivalent_stat(struct ::statx const& s1, struct ::statx const& s2) noexcept
{
    return s1.stx_dev_major == s2.stx_dev_major && s1.stx_dev_minor == s2.stx_dev_minor && s1.stx_ino == s2.stx_ino;
}

//! Returns file type/access mode from \c statx structure
inline mode_t get_mode(struct ::statx const& st) noexcept
{
    return st.stx_mode;
}

//! Returns file size from \c statx structure
inline uintmax_t get_size(struct ::statx const& st) noexcept
{
    return st.stx_size;
}

//! Returns optimal block size from \c statx structure
inline std::size_t get_blksize(struct ::statx const& st) noexcept
{
    return st.stx_blksize;
}

#else // defined(BOOST_FILESYSTEM_USE_STATX)

//! Returns \c true if the two \c stat structures refer to the same file
inline bool equivalent_stat(struct ::stat const& s1, struct ::stat const& s2) noexcept
{
    // According to the POSIX stat specs, "The st_ino and st_dev fields
    // taken together uniquely identify the file within the system."
    return s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino;
}

//! Returns file type/access mode from \c stat structure
inline mode_t get_mode(struct ::stat const& st) noexcept
{
    return st.st_mode;
}

//! Returns file size from \c stat structure
inline uintmax_t get_size(struct ::stat const& st) noexcept
{
    return st.st_size;
}

//! Returns optimal block size from \c stat structure
inline std::size_t get_blksize(struct ::stat const& st) noexcept
{
#if defined(BOOST_FILESYSTEM_HAS_STAT_ST_BLKSIZE)
    return st.st_blksize;
#else
    return 4096u; // a suitable default used on most modern SSDs/HDDs
#endif
}

#endif // defined(BOOST_FILESYSTEM_USE_STATX)

} // namespace

//! status() implementation
file_status status_impl
(
    path const& p,
    system::error_code* ec
#if defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS) || defined(BOOST_FILESYSTEM_USE_STATX)
    , int basedir_fd
#endif
)
{
#if defined(BOOST_FILESYSTEM_USE_STATX)
    struct ::statx path_stat;
    int err = invoke_statx(basedir_fd, p.c_str(), AT_NO_AUTOMOUNT, STATX_TYPE | STATX_MODE, &path_stat);
#elif defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
    struct ::stat path_stat;
    int err = ::fstatat(basedir_fd, p.c_str(), &path_stat, AT_NO_AUTOMOUNT);
#else
    struct ::stat path_stat;
    int err = ::stat(p.c_str(), &path_stat);
#endif

    if (err != 0)
    {
        err = errno;
        if (ec)                                         // always report errno, even though some
            ec->assign(err, system::system_category()); // errno values are not status_errors

        if (not_found_error(err))
            return fs::file_status(fs::file_not_found, fs::no_perms);

        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::status", p, system::error_code(err, system::system_category())));

        return fs::file_status(fs::status_error);
    }

#if defined(BOOST_FILESYSTEM_USE_STATX)
    if (BOOST_UNLIKELY((path_stat.stx_mask & (STATX_TYPE | STATX_MODE)) != (STATX_TYPE | STATX_MODE)))
    {
        emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::status");
        return fs::file_status(fs::status_error);
    }
#endif

    const mode_t mode = get_mode(path_stat);
    if (S_ISDIR(mode))
        return fs::file_status(fs::directory_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISREG(mode))
        return fs::file_status(fs::regular_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISBLK(mode))
        return fs::file_status(fs::block_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISCHR(mode))
        return fs::file_status(fs::character_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISFIFO(mode))
        return fs::file_status(fs::fifo_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISSOCK(mode))
        return fs::file_status(fs::socket_file, static_cast< perms >(mode) & fs::perms_mask);

    return fs::file_status(fs::type_unknown);
}

//! symlink_status() implementation
file_status symlink_status_impl
(
    path const& p,
    system::error_code* ec
#if defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS) || defined(BOOST_FILESYSTEM_USE_STATX)
    , int basedir_fd
#endif
)
{
#if defined(BOOST_FILESYSTEM_USE_STATX)
    struct ::statx path_stat;
    int err = invoke_statx(basedir_fd, p.c_str(), AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_TYPE | STATX_MODE, &path_stat);
#elif defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
    struct ::stat path_stat;
    int err = ::fstatat(basedir_fd, p.c_str(), &path_stat, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT);
#else
    struct ::stat path_stat;
    int err = ::lstat(p.c_str(), &path_stat);
#endif

    if (err != 0)
    {
        err = errno;
        if (ec)                                         // always report errno, even though some
            ec->assign(err, system::system_category()); // errno values are not status_errors

        if (not_found_error(err)) // these are not errors
            return fs::file_status(fs::file_not_found, fs::no_perms);

        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::symlink_status", p, system::error_code(err, system::system_category())));

        return fs::file_status(fs::status_error);
    }

#if defined(BOOST_FILESYSTEM_USE_STATX)
    if (BOOST_UNLIKELY((path_stat.stx_mask & (STATX_TYPE | STATX_MODE)) != (STATX_TYPE | STATX_MODE)))
    {
        emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::symlink_status");
        return fs::file_status(fs::status_error);
    }
#endif

    const mode_t mode = get_mode(path_stat);
    if (S_ISREG(mode))
        return fs::file_status(fs::regular_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISDIR(mode))
        return fs::file_status(fs::directory_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISLNK(mode))
        return fs::file_status(fs::symlink_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISBLK(mode))
        return fs::file_status(fs::block_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISCHR(mode))
        return fs::file_status(fs::character_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISFIFO(mode))
        return fs::file_status(fs::fifo_file, static_cast< perms >(mode) & fs::perms_mask);
    if (S_ISSOCK(mode))
        return fs::file_status(fs::socket_file, static_cast< perms >(mode) & fs::perms_mask);

    return fs::file_status(fs::type_unknown);
}

namespace {

//! Flushes buffered data and attributes written to the file to permanent storage
inline int full_sync(int fd)
{
    while (true)
    {
#if defined(__APPLE__) && defined(__MACH__) && defined(F_FULLFSYNC)
        // Mac OS does not flush data to physical storage with fsync()
        int err = ::fcntl(fd, F_FULLFSYNC);
#else
        int err = ::fsync(fd);
#endif
        if (BOOST_UNLIKELY(err < 0))
        {
            err = errno;
            // POSIX says fsync can return EINTR (https://pubs.opengroup.org/onlinepubs/9699919799/functions/fsync.html).
            // fcntl(F_FULLFSYNC) isn't documented to return EINTR, but it doesn't hurt to check.
            if (err == EINTR)
                continue;

            return err;
        }

        break;
    }

    return 0;
}

//! Flushes buffered data written to the file to permanent storage
inline int data_sync(int fd)
{
#if defined(BOOST_FILESYSTEM_HAS_FDATASYNC) && !(defined(__APPLE__) && defined(__MACH__) && defined(F_FULLFSYNC))
    while (true)
    {
        int err = ::fdatasync(fd);
        if (BOOST_UNLIKELY(err != 0))
        {
            err = errno;
            // POSIX says fsync can return EINTR (https://pubs.opengroup.org/onlinepubs/9699919799/functions/fsync.html).
            // It doesn't say so for fdatasync, but it is reasonable to expect it as well.
            if (err == EINTR)
                continue;

            return err;
        }

        break;
    }

    return 0;
#else
    return full_sync(fd);
#endif
}

//! Hints the filesystem to opportunistically preallocate storage for a file
inline int preallocate_storage(int file, uintmax_t size)
{
#if defined(BOOST_FILESYSTEM_HAS_FALLOCATE)
    if (BOOST_LIKELY(size > 0 && size <= static_cast< uintmax_t >((std::numeric_limits< off_t >::max)())))
    {
        while (true)
        {
            // Note: We intentionally use fallocate rather than posix_fallocate to avoid
            //       invoking glibc emulation that writes zeros to the end of the file.
            //       We want this call to act like a hint to the filesystem and an early
            //       check for the free storage space. We don't want to write zeros only
            //       to later overwrite them with the actual data.
            int err = fallocate(file, FALLOC_FL_KEEP_SIZE, 0, static_cast< off_t >(size));
            if (BOOST_UNLIKELY(err != 0))
            {
                err = errno;

                // Ignore the error if the operation is not supported by the kernel or filesystem
                if (err == EOPNOTSUPP || err == ENOSYS)
                    break;

                if (err == EINTR)
                    continue;

                return err;
            }

            break;
        }
    }
#endif

    return 0;
}

//! copy_file implementation wrapper that preallocates storage for the target file
template< typename CopyFileData >
struct copy_file_data_preallocate
{
    //! copy_file implementation wrapper that preallocates storage for the target file before invoking the underlying copy implementation
    static int impl(int infile, int outfile, uintmax_t size, std::size_t blksize)
    {
        int err = preallocate_storage(outfile, size);
        if (BOOST_UNLIKELY(err != 0))
            return err;

        return CopyFileData::impl(infile, outfile, size, blksize);
    }
};

// Min and max buffer sizes are selected to minimize the overhead from system calls.
// The values are picked based on coreutils cp(1) benchmarking data described here:
// https://github.com/coreutils/coreutils/blob/d1b0257077c0b0f0ee25087efd46270345d1dd1f/src/ioblksize.h#L23-L72
BOOST_CONSTEXPR_OR_CONST uint_least32_t min_read_write_buf_size = 8u * 1024u;
BOOST_CONSTEXPR_OR_CONST uint_least32_t max_read_write_buf_size = 256u * 1024u;

//! copy_file read/write loop implementation
int copy_file_data_read_write_impl(int infile, int outfile, char* buf, std::size_t buf_size)
{
#if defined(BOOST_FILESYSTEM_HAS_POSIX_FADVISE)
    ::posix_fadvise(infile, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    // Don't use file size to limit the amount of data to copy since some filesystems, like procfs or sysfs,
    // provide files with generated content and indicate that their size is zero or 4096. Just copy as much data
    // as we can read from the input file.
    while (true)
    {
        ssize_t sz_read = ::read(infile, buf, buf_size);
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
            ssize_t sz = ::write(outfile, buf + sz_wrote, static_cast< std::size_t >(sz_read - sz_wrote));
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

//! copy_file implementation that uses read/write loop (fallback using a stack buffer)
int copy_file_data_read_write_stack_buf(int infile, int outfile)
{
    char stack_buf[min_read_write_buf_size];
    return copy_file_data_read_write_impl(infile, outfile, stack_buf, sizeof(stack_buf));
}

//! copy_file implementation that uses read/write loop
int copy_file_data_read_write(int infile, int outfile, uintmax_t size, std::size_t blksize)
{
    {
        uintmax_t buf_sz = size;
        // Prefer the buffer to be larger than the file size so that we don't have
        // to perform an extra read if the file fits in the buffer exactly.
        buf_sz += (buf_sz < ~static_cast< uintmax_t >(0u));
        if (buf_sz < blksize)
            buf_sz = blksize;
        if (buf_sz < min_read_write_buf_size)
            buf_sz = min_read_write_buf_size;
        if (buf_sz > max_read_write_buf_size)
            buf_sz = max_read_write_buf_size;
        const std::size_t buf_size = static_cast< std::size_t >(boost::core::bit_ceil(static_cast< uint_least32_t >(buf_sz)));
        std::unique_ptr< char[] > buf(new (std::nothrow) char[buf_size]);
        if (BOOST_LIKELY(!!buf.get()))
            return copy_file_data_read_write_impl(infile, outfile, buf.get(), buf_size);
    }

    return copy_file_data_read_write_stack_buf(infile, outfile);
}

typedef int copy_file_data_t(int infile, int outfile, uintmax_t size, std::size_t blksize);

//! Pointer to the actual implementation of copy_file_data
copy_file_data_t* copy_file_data = &copy_file_data_read_write;

#if defined(BOOST_FILESYSTEM_USE_SENDFILE) || defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)

//! copy_file_data wrapper that tests if a read/write loop must be used for a given filesystem
template< typename CopyFileData >
int check_fs_type(int infile, int outfile, uintmax_t size, std::size_t blksize);

#endif // defined(BOOST_FILESYSTEM_USE_SENDFILE) || defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)

#if defined(BOOST_FILESYSTEM_USE_SENDFILE)

struct copy_file_data_sendfile
{
    //! copy_file implementation that uses sendfile loop. Requires sendfile to support file descriptors.
    static int impl(int infile, int outfile, uintmax_t size, std::size_t blksize)
    {
        // sendfile will not send more than this amount of data in one call
        BOOST_CONSTEXPR_OR_CONST std::size_t max_batch_size = 0x7ffff000u;
        uintmax_t offset = 0u;
        while (offset < size)
        {
            uintmax_t size_left = size - offset;
            std::size_t size_to_copy = max_batch_size;
            if (size_left < static_cast< uintmax_t >(max_batch_size))
                size_to_copy = static_cast< std::size_t >(size_left);
            ssize_t sz = ::sendfile(outfile, infile, nullptr, size_to_copy);
            if (BOOST_LIKELY(sz > 0))
            {
                offset += sz;
            }
            else if (sz < 0)
            {
                int err = errno;
                if (err == EINTR)
                    continue;

                if (offset == 0u)
                {
                    // sendfile may fail with EINVAL if the underlying filesystem does not support it
                    if (err == EINVAL)
                    {
                    fallback_to_read_write:
                        return copy_file_data_read_write(infile, outfile, size, blksize);
                    }

                    if (err == ENOSYS)
                    {
                        filesystem::detail::atomic_store_relaxed(copy_file_data, &copy_file_data_read_write);
                        goto fallback_to_read_write;
                    }
                }

                return err;
            }
            else
            {
                // EOF: the input file was truncated while copying was in progress
                break;
            }
        }

        return 0;
    }
};

#endif // defined(BOOST_FILESYSTEM_USE_SENDFILE)

#if defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)

struct copy_file_data_copy_file_range
{
    //! copy_file implementation that uses copy_file_range loop. Requires copy_file_range to support cross-filesystem copying.
    static int impl(int infile, int outfile, uintmax_t size, std::size_t blksize)
    {
        // Although copy_file_range does not document any particular upper limit of one transfer, still use some upper bound to guarantee
        // that size_t is not overflown in case if off_t is larger and the file size does not fit in size_t.
        BOOST_CONSTEXPR_OR_CONST std::size_t max_batch_size = 0x7ffff000u;
        uintmax_t offset = 0u;
        while (offset < size)
        {
            uintmax_t size_left = size - offset;
            std::size_t size_to_copy = max_batch_size;
            if (size_left < static_cast< uintmax_t >(max_batch_size))
                size_to_copy = static_cast< std::size_t >(size_left);
            // Note: Use syscall directly to avoid depending on libc version. copy_file_range is added in glibc 2.27.
            // uClibc-ng does not have copy_file_range as of the time of this writing (the latest uClibc-ng release is 1.0.33).
            loff_t sz = ::syscall(__NR_copy_file_range, infile, (loff_t*)nullptr, outfile, (loff_t*)nullptr, size_to_copy, (unsigned int)0u);
            if (BOOST_LIKELY(sz > 0))
            {
                offset += sz;
            }
            else if (sz < 0)
            {
                int err = errno;
                if (err == EINTR)
                    continue;

                if (offset == 0u)
                {
                    // copy_file_range may fail with EINVAL if the underlying filesystem does not support it.
                    // In some RHEL/CentOS 7.7-7.8 kernel versions, copy_file_range on NFSv4 is also known to return EOPNOTSUPP
                    // if the remote server does not support COPY, despite that it is not a documented error code.
                    // See https://patchwork.kernel.org/project/linux-nfs/patch/20190411183418.4510-1-olga.kornievskaia@gmail.com/
                    // and https://bugzilla.redhat.com/show_bug.cgi?id=1783554.
                    if (err == EINVAL || err == EOPNOTSUPP)
                    {
#if !defined(BOOST_FILESYSTEM_USE_SENDFILE)
                    fallback_to_read_write:
#endif
                        return copy_file_data_read_write(infile, outfile, size, blksize);
                    }

                    if (err == EXDEV)
                    {
#if defined(BOOST_FILESYSTEM_USE_SENDFILE)
                    fallback_to_sendfile:
                        return copy_file_data_sendfile::impl(infile, outfile, size, blksize);
#else
                        goto fallback_to_read_write;
#endif
                    }

                    if (err == ENOSYS)
                    {
#if defined(BOOST_FILESYSTEM_USE_SENDFILE)
                        filesystem::detail::atomic_store_relaxed(copy_file_data, &check_fs_type< copy_file_data_preallocate< copy_file_data_sendfile > >);
                        goto fallback_to_sendfile;
#else
                        filesystem::detail::atomic_store_relaxed(copy_file_data, &copy_file_data_read_write);
                        goto fallback_to_read_write;
#endif
                    }
                }

                return err;
            }
            else
            {
                // EOF: the input file was truncated while copying was in progress
                break;
            }
        }

        return 0;
    }
};

#endif // defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)

#if defined(BOOST_FILESYSTEM_USE_SENDFILE) || defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)

//! copy_file_data wrapper that tests if a read/write loop must be used for a given filesystem
template< typename CopyFileData >
int check_fs_type(int infile, int outfile, uintmax_t size, std::size_t blksize)
{
    {
        // Some filesystems have regular files with generated content. Such files have arbitrary size, including zero,
        // but have actual content. Linux system calls sendfile or copy_file_range will not copy contents of such files,
        // so we must use a read/write loop to handle them.
        // https://lore.kernel.org/linux-fsdevel/20210212044405.4120619-1-drinkcat@chromium.org/T/
        struct statfs sfs;
        while (true)
        {
            int err = ::fstatfs(infile, &sfs);
            if (BOOST_UNLIKELY(err < 0))
            {
                err = errno;
                if (err == EINTR)
                    continue;

                goto fallback_to_read_write;
            }

            break;
        }

        if (BOOST_UNLIKELY(sfs.f_type == PROC_SUPER_MAGIC ||
            sfs.f_type == SYSFS_MAGIC ||
            sfs.f_type == TRACEFS_MAGIC ||
            sfs.f_type == DEBUGFS_MAGIC))
        {
        fallback_to_read_write:
            return copy_file_data_read_write(infile, outfile, size, blksize);
        }
    }

    return CopyFileData::impl(infile, outfile, size, blksize);
}

#endif // defined(BOOST_FILESYSTEM_USE_SENDFILE) || defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)

#if defined(linux) || defined(__linux) || defined(__linux__)

//! Initializes copy_file_data implementation pointer
inline void init_copy_file_data_impl(unsigned int major_ver, unsigned int minor_ver, unsigned int patch_ver)
{
#if defined(BOOST_FILESYSTEM_USE_SENDFILE) || defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)
    copy_file_data_t* cfd = &copy_file_data_read_write;

#if defined(BOOST_FILESYSTEM_USE_SENDFILE)
    // sendfile started accepting file descriptors as the target in Linux 2.6.33
    if (major_ver > 2u || (major_ver == 2u && (minor_ver > 6u || (minor_ver == 6u && patch_ver >= 33u))))
        cfd = &check_fs_type< copy_file_data_preallocate< copy_file_data_sendfile > >;
#endif

#if defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)
    // Although copy_file_range appeared in Linux 4.5, it did not support cross-filesystem copying until 5.3.
    // copy_file_data_copy_file_range will fallback to copy_file_data_sendfile if copy_file_range returns EXDEV.
    if (major_ver > 4u || (major_ver == 4u && minor_ver >= 5u))
        cfd = &check_fs_type< copy_file_data_preallocate< copy_file_data_copy_file_range > >;
#endif

    filesystem::detail::atomic_store_relaxed(copy_file_data, cfd);
#endif // defined(BOOST_FILESYSTEM_USE_SENDFILE) || defined(BOOST_FILESYSTEM_USE_COPY_FILE_RANGE)
}

#endif // defined(linux) || defined(__linux) || defined(__linux__)

#if defined(linux) || defined(__linux) || defined(__linux__)

struct syscall_initializer
{
    syscall_initializer()
    {
        struct ::utsname system_info;
        if (BOOST_UNLIKELY(::uname(&system_info) < 0))
            return;

        unsigned int major_ver = 0u, minor_ver = 0u, patch_ver = 0u;
        int count = std::sscanf(system_info.release, "%u.%u.%u", &major_ver, &minor_ver, &patch_ver);
        if (BOOST_UNLIKELY(count < 3))
            return;

        init_statx_impl(major_ver, minor_ver, patch_ver);
        init_copy_file_data_impl(major_ver, minor_ver, patch_ver);
        init_fill_random_impl(major_ver, minor_ver, patch_ver);
    }
};

BOOST_FILESYSTEM_INIT_PRIORITY(BOOST_FILESYSTEM_FUNC_PTR_INIT_PRIORITY) BOOST_ATTRIBUTE_UNUSED BOOST_FILESYSTEM_ATTRIBUTE_RETAIN
const syscall_initializer syscall_init;

#endif // defined(linux) || defined(__linux) || defined(__linux__)

//! remove() implementation
inline bool remove_impl
(
    path const& p,
    fs::file_type type,
    error_code* ec
#if defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
    , int basedir_fd = AT_FDCWD
#endif
)
{
    if (type == fs::file_not_found)
        return false;

    int res;
#if defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
    res = ::unlinkat(basedir_fd, p.c_str(), type == fs::directory_file ? AT_REMOVEDIR : 0);
#else
    if (type == fs::directory_file)
        res = ::rmdir(p.c_str());
    else
        res = ::unlink(p.c_str());
#endif

    if (res != 0)
    {
        int err = errno;
        if (BOOST_UNLIKELY(!not_found_error(err)))
            emit_error(err, p, ec, "boost::filesystem::remove");

        return false;
    }

    return true;
}

//! remove() implementation
inline bool remove_impl(path const& p, error_code* ec)
{
    // Since POSIX remove() is specified to work with either files or directories, in a
    // perfect world it could just be called. But some important real-world operating
    // systems (Windows, Mac OS, for example) don't implement the POSIX spec. So
    // we have to distinguish between files and directories and call corresponding APIs
    // to remove them.

    error_code local_ec;
    fs::file_type type = fs::detail::symlink_status_impl(p, &local_ec).type();
    if (BOOST_UNLIKELY(type == fs::status_error))
    {
        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::remove", p, local_ec));

        *ec = local_ec;
        return false;
    }

    return fs::detail::remove_impl(p, type, ec);
}

//! remove_all() implementation
uintmax_t remove_all_impl
(
    path const& p,
    error_code* ec
#if defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW) && defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
    , int parentdir_fd = AT_FDCWD
#endif
)
{
#if defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW) && defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
    fs::path filename;
    const fs::path* remove_path = &p;
    if (parentdir_fd != AT_FDCWD)
    {
        filename = path_algorithms::filename_v4(p);
        remove_path = &filename;
    }
#endif

    error_code dit_create_ec;
    for (unsigned int attempt = 0u; attempt < remove_all_directory_replaced_retry_count; ++attempt)
    {
        fs::file_type type;
        {
            error_code local_ec;
#if defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW) && defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
            type = fs::detail::symlink_status_impl(*remove_path, &local_ec, parentdir_fd).type();
#else
            type = fs::detail::symlink_status_impl(p, &local_ec).type();
#endif

            if (type == fs::file_not_found)
                return 0u;

            if (BOOST_UNLIKELY(type == fs::status_error))
            {
                if (!ec)
                    BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::remove_all", p, local_ec));

                *ec = local_ec;
                return static_cast< uintmax_t >(-1);
            }
        }

        uintmax_t count = 0u;
        if (type == fs::directory_file) // but not a directory symlink
        {
            fs::directory_iterator itr;
#if defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW) && defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
            fs::detail::directory_iterator_params params{ fs::detail::openat_directory(parentdir_fd, *remove_path, directory_options::_detail_no_follow, dit_create_ec) };
            int dir_fd = -1;
            if (BOOST_LIKELY(!dit_create_ec))
            {
                // Save dir_fd as constructing the iterator will move the fd into the iterator context
                dir_fd = params.dir_fd.get();
                fs::detail::directory_iterator_construct(itr, *remove_path, directory_options::_detail_no_follow, &params, &dit_create_ec);
            }
#else
            fs::detail::directory_iterator_construct
            (
                itr,
                p,
#if defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW)
                directory_options::_detail_no_follow,
#else
                directory_options::none,
#endif
                nullptr,
                &dit_create_ec
            );
#endif

            if (BOOST_UNLIKELY(!!dit_create_ec))
            {
                if (dit_create_ec == error_code(ENOTDIR, system_category()))
                    continue;

#if defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW)
                // If open(2) with O_NOFOLLOW fails with ELOOP, this means that either the path contains a loop
                // of symbolic links, or the last element of the path is a symbolic link. Given that lstat(2) above
                // did not fail, most likely it is the latter case. I.e. between the lstat above and this open call
                // the filesystem was modified so that the path no longer refers to a directory file (as opposed to a symlink).
                if (dit_create_ec == error_code(ELOOP, system_category()))
                    continue;
#endif // defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW)

                if (!ec)
                    BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::remove_all", p, dit_create_ec));

                *ec = dit_create_ec;
                return static_cast< uintmax_t >(-1);
            }

            const fs::directory_iterator end_dit;
            while (itr != end_dit)
            {
                count += fs::detail::remove_all_impl
                (
                    itr->path(),
                    ec
#if defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW) && defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
                    , dir_fd
#endif
                );
                if (ec && *ec)
                    return static_cast< uintmax_t >(-1);

                fs::detail::directory_iterator_increment(itr, ec);
                if (ec && *ec)
                    return static_cast< uintmax_t >(-1);
            }
        }

#if defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW) && defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
        count += fs::detail::remove_impl(*remove_path, type, ec, parentdir_fd);
#else
        count += fs::detail::remove_impl(p, type, ec);
#endif
        if (ec && *ec)
            return static_cast< uintmax_t >(-1);

        return count;
    }

    if (!ec)
        BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::remove_all: path cannot be opened as a directory", p, dit_create_ec));

    *ec = dit_create_ec;
    return static_cast< uintmax_t >(-1);
}

#else // defined(BOOST_POSIX_API)

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            Windows-specific helpers                                  //
//                                                                                      //
//--------------------------------------------------------------------------------------//

//! FILE_BASIC_INFO definition from Windows SDK
struct file_basic_info
{
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    DWORD FileAttributes;
};

//! FILE_DISPOSITION_INFO definition from Windows SDK
struct file_disposition_info
{
    BOOLEAN DeleteFile;
};

//! FILE_DISPOSITION_INFO_EX definition from Windows SDK
struct file_disposition_info_ex
{
    DWORD Flags;
};

#ifndef FILE_DISPOSITION_FLAG_DELETE
#define FILE_DISPOSITION_FLAG_DELETE 0x00000001
#endif
// Available since Windows 10 1709
#ifndef FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x00000002
#endif
// Available since Windows 10 1809
#ifndef FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE
#define FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE 0x00000010
#endif

//  REPARSE_DATA_BUFFER related definitions are found in ntifs.h, which is part of the
//  Windows Device Driver Kit. Since that's inconvenient, the definitions are provided
//  here. See http://msdn.microsoft.com/en-us/library/ms791514.aspx
struct reparse_data_buffer
{
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union
    {
        /*
         * In SymbolicLink and MountPoint reparse points, there are two names.
         * SubstituteName is the effective replacement path for the reparse point.
         * This is what should be used for path traversal.
         * PrintName is intended for presentation to the user and may omit some
         * elements of the path or be absent entirely.
         *
         * Examples of substitute and print names:
         * mklink /D ldrive c:\
         * SubstituteName: "\??\c:\"
         * PrintName: "c:\"
         *
         * mklink /J ldrive c:\
         * SubstituteName: "\??\C:\"
         * PrintName: "c:\"
         *
         * junction ldrive c:\
         * SubstituteName: "\??\C:\"
         * PrintName: ""
         *
         * box.com mounted cloud storage
         * SubstituteName: "\??\Volume{<UUID>}\"
         * PrintName: ""
         */
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPointReparseBuffer;
        struct
        {
            UCHAR DataBuffer[1];
        } GenericReparseBuffer;
    };
};

// Our convenience type for allocating REPARSE_DATA_BUFFER along with sufficient space after it
union reparse_data_buffer_with_storage
{
    reparse_data_buffer rdb;
    unsigned char storage[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
};

//  Windows kernel32.dll functions that may or may not be present
//  must be accessed through pointers

typedef BOOL (WINAPI CreateHardLinkW_t)(
    /*__in*/ LPCWSTR lpFileName,
    /*__in*/ LPCWSTR lpExistingFileName,
    /*__reserved*/ LPSECURITY_ATTRIBUTES lpSecurityAttributes);

CreateHardLinkW_t* create_hard_link_api = nullptr;

typedef BOOLEAN (WINAPI CreateSymbolicLinkW_t)(
    /*__in*/ LPCWSTR lpSymlinkFileName,
    /*__in*/ LPCWSTR lpTargetFileName,
    /*__in*/ DWORD dwFlags);

CreateSymbolicLinkW_t* create_symbolic_link_api = nullptr;

//! SetFileInformationByHandle signature. Available since Windows Vista.
typedef BOOL (WINAPI SetFileInformationByHandle_t)(
    /*_In_*/ HANDLE hFile,
    /*_In_*/ file_info_by_handle_class FileInformationClass, // the actual type is FILE_INFO_BY_HANDLE_CLASS enum
    /*_In_reads_bytes_(dwBufferSize)*/ LPVOID lpFileInformation,
    /*_In_*/ DWORD dwBufferSize);

SetFileInformationByHandle_t* set_file_information_by_handle_api = nullptr;

} // unnamed namespace

GetFileInformationByHandleEx_t* get_file_information_by_handle_ex_api = nullptr;

NtCreateFile_t* nt_create_file_api = nullptr;
NtQueryDirectoryFile_t* nt_query_directory_file_api = nullptr;

namespace {

//! remove() implementation type
enum remove_impl_type
{
    remove_nt5,                            //!< Use Windows XP API
    remove_disp,                           //!< Use FILE_DISPOSITION_INFO (Windows Vista and later)
    remove_disp_ex_flag_posix_semantics,   //!< Use FILE_DISPOSITION_INFO_EX with FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
    remove_disp_ex_flag_ignore_readonly    //!< Use FILE_DISPOSITION_INFO_EX with FILE_DISPOSITION_FLAG_POSIX_SEMANTICS | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE
};

remove_impl_type g_remove_impl_type = remove_nt5;

//! Initializes WinAPI function pointers
BOOST_FILESYSTEM_INIT_FUNC init_winapi_func_ptrs()
{
    boost::winapi::HMODULE_ h = boost::winapi::GetModuleHandleW(L"kernel32.dll");
    if (BOOST_LIKELY(!!h))
    {
        GetFileInformationByHandleEx_t* get_file_information_by_handle_ex = boost::winapi::get_proc_address<GetFileInformationByHandleEx_t*>(h, "GetFileInformationByHandleEx");
        filesystem::detail::atomic_store_relaxed(get_file_information_by_handle_ex_api, get_file_information_by_handle_ex);
        SetFileInformationByHandle_t* set_file_information_by_handle = boost::winapi::get_proc_address<SetFileInformationByHandle_t*>(h, "SetFileInformationByHandle");
        filesystem::detail::atomic_store_relaxed(set_file_information_by_handle_api, set_file_information_by_handle);
        filesystem::detail::atomic_store_relaxed(create_hard_link_api, boost::winapi::get_proc_address<CreateHardLinkW_t*>(h, "CreateHardLinkW"));
        filesystem::detail::atomic_store_relaxed(create_symbolic_link_api, boost::winapi::get_proc_address<CreateSymbolicLinkW_t*>(h, "CreateSymbolicLinkW"));

        if (get_file_information_by_handle_ex && set_file_information_by_handle)
        {
            // Enable the most advanced implementation based on GetFileInformationByHandleEx/SetFileInformationByHandle.
            // If certain flags are not supported by the OS, the remove() implementation will downgrade accordingly.
            filesystem::detail::atomic_store_relaxed(g_remove_impl_type, remove_disp_ex_flag_ignore_readonly);
        }
    }

    h = boost::winapi::GetModuleHandleW(L"ntdll.dll");
    if (BOOST_LIKELY(!!h))
    {
        filesystem::detail::atomic_store_relaxed(nt_create_file_api, boost::winapi::get_proc_address<NtCreateFile_t*>(h, "NtCreateFile"));
        filesystem::detail::atomic_store_relaxed(nt_query_directory_file_api, boost::winapi::get_proc_address<NtQueryDirectoryFile_t*>(h, "NtQueryDirectoryFile"));
    }

    init_directory_iterator_impl();

    return BOOST_FILESYSTEM_INITRETSUCCESS_V;
}

#if defined(_MSC_VER)

#if _MSC_VER >= 1400

#pragma section(".CRT$XCL", long, read)
__declspec(allocate(".CRT$XCL")) BOOST_ATTRIBUTE_UNUSED BOOST_FILESYSTEM_ATTRIBUTE_RETAIN
extern const init_func_ptr_t p_init_winapi_func_ptrs = &init_winapi_func_ptrs;

#else // _MSC_VER >= 1400

#if (_MSC_VER >= 1300) // 1300 == VC++ 7.0
#pragma data_seg(push, old_seg)
#endif
#pragma data_seg(".CRT$XCL")
BOOST_ATTRIBUTE_UNUSED BOOST_FILESYSTEM_ATTRIBUTE_RETAIN
extern const init_func_ptr_t p_init_winapi_func_ptrs = &init_winapi_func_ptrs;
#pragma data_seg()
#if (_MSC_VER >= 1300) // 1300 == VC++ 7.0
#pragma data_seg(pop, old_seg)
#endif

#endif // _MSC_VER >= 1400

#if defined(BOOST_FILESYSTEM_NO_ATTRIBUTE_RETAIN)
//! Makes sure the global initializer pointers are referenced and not removed by linker
struct globals_retainer
{
    const init_func_ptr_t* volatile m_p_init_winapi_func_ptrs;

    globals_retainer() { m_p_init_winapi_func_ptrs = &p_init_winapi_func_ptrs; }
};
BOOST_ATTRIBUTE_UNUSED
const globals_retainer g_globals_retainer;
#endif // defined(BOOST_FILESYSTEM_NO_ATTRIBUTE_RETAIN)

#else // defined(_MSC_VER)

//! Invokes WinAPI function pointers initialization
struct winapi_func_ptrs_initializer
{
    winapi_func_ptrs_initializer() { init_winapi_func_ptrs(); }
};

BOOST_FILESYSTEM_INIT_PRIORITY(BOOST_FILESYSTEM_FUNC_PTR_INIT_PRIORITY) BOOST_ATTRIBUTE_UNUSED BOOST_FILESYSTEM_ATTRIBUTE_RETAIN
const winapi_func_ptrs_initializer winapi_func_ptrs_init;

#endif // defined(_MSC_VER)


inline std::wstring wgetenv(const wchar_t* name)
{
    // use a separate buffer since C++03 basic_string is not required to be contiguous
    const DWORD size = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (size > 0)
    {
        std::unique_ptr< wchar_t[] > buf(new wchar_t[size]);
        if (BOOST_LIKELY(::GetEnvironmentVariableW(name, buf.get(), size) > 0))
            return std::wstring(buf.get());
    }

    return std::wstring();
}

inline bool not_found_error(int errval) noexcept
{
    return errval == ERROR_FILE_NOT_FOUND || errval == ERROR_PATH_NOT_FOUND || errval == ERROR_INVALID_NAME // "tools/jam/src/:sys:stat.h", "//foo"
        || errval == ERROR_INVALID_DRIVE                                                                    // USB card reader with no card inserted
        || errval == ERROR_NOT_READY                                                                        // CD/DVD drive with no disc inserted
        || errval == ERROR_INVALID_PARAMETER                                                                // ":sys:stat.h"
        || errval == ERROR_BAD_PATHNAME                                                                     // "//no-host" on Win64
        || errval == ERROR_BAD_NETPATH                                                                      // "//no-host" on Win32
        || errval == ERROR_BAD_NET_NAME;                                                                    // "//no-host/no-share" on Win10 x64
}

// these constants come from inspecting some Microsoft sample code
inline DWORD to_time_t(FILETIME const& ft, std::time_t& t)
{
    uint64_t ut = (static_cast< uint64_t >(ft.dwHighDateTime) << 32u) | ft.dwLowDateTime;
    if (BOOST_UNLIKELY(ut > static_cast< uint64_t >((std::numeric_limits< int64_t >::max)())))
        return ERROR_INVALID_DATA;

    // On Windows, time_t is signed, and negative values are possible since FILETIME epoch is earlier than POSIX epoch
    int64_t st = static_cast< int64_t >(ut) / 10000000 - 11644473600ll;
    if (BOOST_UNLIKELY(st < static_cast< int64_t >((std::numeric_limits< std::time_t >::min)()) ||
        st > static_cast< int64_t >((std::numeric_limits< std::time_t >::max)())))
    {
        return ERROR_INVALID_DATA;
    }

    t = static_cast< std::time_t >(st);
    return 0u;
}

inline DWORD to_FILETIME(std::time_t t, FILETIME& ft)
{
    // On Windows, time_t is signed, and negative values are possible since FILETIME epoch is earlier than POSIX epoch
    int64_t st = static_cast< int64_t >(t);
    if (BOOST_UNLIKELY(st < ((std::numeric_limits< int64_t >::min)() / 10000000 - 11644473600ll) ||
        st > ((std::numeric_limits< int64_t >::max)() / 10000000 - 11644473600ll)))
    {
        return ERROR_INVALID_DATA;
    }

    st = (st + 11644473600ll) * 10000000;
    uint64_t ut = static_cast< uint64_t >(st);
    ft.dwLowDateTime = static_cast< DWORD >(ut);
    ft.dwHighDateTime = static_cast< DWORD >(ut >> 32u);

    return 0u;
}

} // unnamed namespace

//! The flag indicates whether OBJ_DONT_REPARSE flag is not supported by the kernel
static bool g_no_obj_dont_reparse = false;

//! Creates a file handle for a file relative to a previously opened base directory. The file path must be relative and in preferred format.
boost::winapi::NTSTATUS_ nt_create_file_handle_at
(
    unique_handle& out,
    HANDLE basedir_handle,
    boost::filesystem::path const& p,
    ULONG FileAttributes,
    ACCESS_MASK DesiredAccess,
    ULONG ShareMode,
    ULONG CreateDisposition,
    ULONG CreateOptions
)
{
    NtCreateFile_t* nt_create_file = filesystem::detail::atomic_load_relaxed(nt_create_file_api);
    if (BOOST_UNLIKELY(!nt_create_file))
        return static_cast< boost::winapi::NTSTATUS_ >(STATUS_NOT_IMPLEMENTED);

    unicode_string obj_name = {};
    obj_name.Buffer = const_cast< wchar_t* >(p.c_str());
    obj_name.Length = obj_name.MaximumLength = static_cast< USHORT >(p.size() * sizeof(wchar_t));

    object_attributes obj_attrs = {};
    obj_attrs.Length = sizeof(obj_attrs);
    obj_attrs.RootDirectory = basedir_handle;
    obj_attrs.ObjectName = &obj_name;

    obj_attrs.Attributes = OBJ_CASE_INSENSITIVE;
    if ((CreateOptions & FILE_OPEN_REPARSE_POINT) != 0u && !filesystem::detail::atomic_load_relaxed(g_no_obj_dont_reparse))
        obj_attrs.Attributes |= OBJ_DONT_REPARSE;

    io_status_block iosb;
    HANDLE out_handle = INVALID_HANDLE_VALUE;
    boost::winapi::NTSTATUS_ status = nt_create_file
    (
        &out_handle,
        DesiredAccess,
        &obj_attrs,
        &iosb,
        nullptr, // AllocationSize
        FileAttributes,
        ShareMode,
        CreateDisposition,
        CreateOptions,
        nullptr, // EaBuffer
        0u // EaLength
    );

    if (BOOST_UNLIKELY(BOOST_NTSTATUS_EQ(status, STATUS_INVALID_PARAMETER) && (obj_attrs.Attributes & OBJ_DONT_REPARSE) != 0u))
    {
        // OBJ_DONT_REPARSE is supported since Windows 10, retry without it
        filesystem::detail::atomic_store_relaxed(g_no_obj_dont_reparse, true);
        obj_attrs.Attributes &= ~static_cast< ULONG >(OBJ_DONT_REPARSE);

        status = nt_create_file
        (
            &out_handle,
            DesiredAccess,
            &obj_attrs,
            &iosb,
            nullptr, // AllocationSize
            FileAttributes,
            ShareMode,
            CreateDisposition,
            CreateOptions,
            nullptr, // EaBuffer
            0u // EaLength
        );
    }

    out.reset(out_handle);

    return status;
}

ULONG get_reparse_point_tag_ioctl(HANDLE h, path const& p, error_code* ec)
{
    std::unique_ptr< reparse_data_buffer_with_storage > buf(new (std::nothrow) reparse_data_buffer_with_storage);
    if (BOOST_UNLIKELY(!buf.get()))
    {
        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("Cannot allocate memory to query reparse point", p, make_error_code(system::errc::not_enough_memory)));

        *ec = make_error_code(system::errc::not_enough_memory);
        return 0u;
    }

    // Query the reparse data
    DWORD dwRetLen = 0u;
    BOOL result = ::DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.get(), sizeof(*buf), &dwRetLen, nullptr);
    if (BOOST_UNLIKELY(!result))
    {
        DWORD err = ::GetLastError();
        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("Failed to query reparse point", p, error_code(err, system_category())));

        ec->assign(err, system_category());
        return 0u;
    }

    return buf->rdb.ReparseTag;
}

namespace {

inline std::size_t get_full_path_name(path const& src, std::size_t len, wchar_t* buf, wchar_t** p)
{
    return static_cast< std::size_t >(::GetFullPathNameW(src.c_str(), static_cast< DWORD >(len), buf, p));
}

inline fs::file_status process_status_failure(DWORD errval, path const& p, error_code* ec)
{
    if (ec)                                    // always report errval, even though some
        ec->assign(errval, system_category()); // errval values are not status_errors

    if (not_found_error(errval))
    {
        return fs::file_status(fs::file_not_found, fs::no_perms);
    }
    else if (errval == ERROR_SHARING_VIOLATION)
    {
        return fs::file_status(fs::type_unknown);
    }

    if (!ec)
        BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::status", p, error_code(errval, system_category())));

    return fs::file_status(fs::status_error);
}

inline fs::file_status process_status_failure(path const& p, error_code* ec)
{
    return process_status_failure(::GetLastError(), p, ec);
}

} // namespace

//! (symlink_)status() by handle implementation
fs::file_status status_by_handle(HANDLE h, path const& p, error_code* ec)
{
    fs::file_type ftype;
    DWORD attrs;
    ULONG reparse_tag = 0u;
    GetFileInformationByHandleEx_t* get_file_information_by_handle_ex = filesystem::detail::atomic_load_relaxed(get_file_information_by_handle_ex_api);
    if (BOOST_LIKELY(get_file_information_by_handle_ex != nullptr))
    {
        file_attribute_tag_info info;
        BOOL res = get_file_information_by_handle_ex(h, file_attribute_tag_info_class, &info, sizeof(info));
        if (BOOST_UNLIKELY(!res))
        {
            // On FAT/exFAT filesystems requesting FILE_ATTRIBUTE_TAG_INFO returns ERROR_INVALID_PARAMETER.
            // Presumably, this is because these filesystems don't support reparse points, so ReparseTag
            // cannot be returned. Also check ERROR_NOT_SUPPORTED for good measure. Fall back to the legacy
            // code path in this case.
            DWORD err = ::GetLastError();
            if (err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED)
                goto use_get_file_information_by_handle;

            return process_status_failure(err, p, ec);
        }

        attrs = info.FileAttributes;
        reparse_tag = info.ReparseTag;
    }
    else
    {
    use_get_file_information_by_handle:
        BY_HANDLE_FILE_INFORMATION info;
        BOOL res = ::GetFileInformationByHandle(h, &info);
        if (BOOST_UNLIKELY(!res))
            return process_status_failure(p, ec);

        attrs = info.dwFileAttributes;

        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        {
            reparse_tag = get_reparse_point_tag_ioctl(h, p, ec);
            if (ec)
            {
                if (BOOST_UNLIKELY(!!ec))
                    return fs::file_status(fs::status_error);
            }
        }
    }

    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
    {
        if (reparse_tag == IO_REPARSE_TAG_DEDUP)
            ftype = fs::regular_file;
        else if (is_reparse_point_tag_a_symlink(reparse_tag))
            ftype = fs::symlink_file;
        else
            ftype = fs::reparse_file;
    }
    else if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0u)
    {
        ftype = fs::directory_file;
    }
    else
    {
        ftype = fs::regular_file;
    }

    return fs::file_status(ftype, make_permissions(p, attrs));
}

namespace {

//! symlink_status() implementation
fs::file_status symlink_status_impl(path const& p, error_code* ec)
{
    // Normally, we only need FILE_READ_ATTRIBUTES access mode. But SMBv1 reports incorrect
    // file attributes in GetFileInformationByHandleEx in this case (e.g. it reports FILE_ATTRIBUTE_NORMAL
    // for a directory in a SMBv1 share), so we add FILE_READ_EA as a workaround.
    // https://github.com/boostorg/filesystem/issues/282
    unique_handle h(create_file_handle(
        p.c_str(),
        FILE_READ_ATTRIBUTES | FILE_READ_EA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, // lpSecurityAttributes
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));

    if (!h)
    {
        // For some system files and folders like "System Volume Information" CreateFileW fails
        // with ERROR_ACCESS_DENIED. GetFileAttributesW succeeds for such files, so try that.
        // Though this will only help if the file is not a reparse point (symlink or not).
        DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED)
        {
            DWORD attrs = ::GetFileAttributesW(p.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES)
            {
                if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0u)
                    return fs::file_status((attrs & FILE_ATTRIBUTE_DIRECTORY) ? fs::directory_file : fs::regular_file, make_permissions(p, attrs));
            }
            else
            {
                err = ::GetLastError();
            }
        }

        return process_status_failure(err, p, ec);
    }

    return detail::status_by_handle(h.get(), p, ec);
}

//! status() implementation
fs::file_status status_impl(path const& p, error_code* ec)
{
    // We should first test if the file is a symlink or a reparse point. Resolving some reparse
    // points by opening the file may fail, and status() should return file_status(reparse_file) in this case.
    // Which is what symlink_status() returns.
    fs::file_status st(detail::symlink_status_impl(p, ec));
    if (st.type() == symlink_file)
    {
        // Resolve the symlink
        unique_handle h(create_file_handle(
            p.c_str(),
            FILE_READ_ATTRIBUTES | FILE_READ_EA, // see the comment in symlink_status_impl re. access mode
            FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, // lpSecurityAttributes
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS));

        if (!h)
            return process_status_failure(p, ec);

        st = detail::status_by_handle(h.get(), p, ec);
    }

    return st;
}

//! remove() implementation for Windows XP and older
bool remove_nt5_impl(path const& p, DWORD attrs, error_code* ec)
{
    const bool is_directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool is_read_only = (attrs & FILE_ATTRIBUTE_READONLY) != 0;
    if (is_read_only)
    {
        // RemoveDirectoryW and DeleteFileW do not allow to remove a read-only file, so we have to drop the attribute
        DWORD new_attrs = attrs & ~FILE_ATTRIBUTE_READONLY;
        BOOL res = ::SetFileAttributesW(p.c_str(), new_attrs);
        if (BOOST_UNLIKELY(!res))
        {
            DWORD err = ::GetLastError();
            if (!not_found_error(err))
                emit_error(err, p, ec, "boost::filesystem::remove");

            return false;
        }
    }

    BOOL res;
    if (!is_directory)
    {
        // DeleteFileW works for file symlinks by removing the symlink, not the target.
        res = ::DeleteFileW(p.c_str());
    }
    else
    {
        // RemoveDirectoryW works for symlinks and junctions by removing the symlink, not the target,
        // even if the target directory is not empty.
        // Note that unlike opening the directory with FILE_FLAG_DELETE_ON_CLOSE flag, RemoveDirectoryW
        // will fail if the directory is not empty.
        res = ::RemoveDirectoryW(p.c_str());
    }

    if (BOOST_UNLIKELY(!res))
    {
        DWORD err = ::GetLastError();
        if (!not_found_error(err))
        {
            if (is_read_only)
            {
                // Try to restore the read-only attribute
                ::SetFileAttributesW(p.c_str(), attrs);
            }

            emit_error(err, p, ec, "boost::filesystem::remove");
        }

        return false;
    }

    return true;
}

//! remove() by handle implementation for Windows Vista and newer
DWORD remove_nt6_by_handle(HANDLE handle, remove_impl_type impl)
{
    GetFileInformationByHandleEx_t* get_file_information_by_handle_ex = filesystem::detail::atomic_load_relaxed(get_file_information_by_handle_ex_api);
    SetFileInformationByHandle_t* set_file_information_by_handle = filesystem::detail::atomic_load_relaxed(set_file_information_by_handle_api);
    DWORD err = 0u;
    switch (impl)
    {
    case remove_disp_ex_flag_ignore_readonly:
        {
            file_disposition_info_ex info;
            info.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
            BOOL res = set_file_information_by_handle(handle, file_disposition_info_ex_class, &info, sizeof(info));
            if (BOOST_LIKELY(!!res))
                break;

            err = ::GetLastError();
            if (BOOST_UNLIKELY(err == ERROR_INVALID_PARAMETER || err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED || err == ERROR_CALL_NOT_IMPLEMENTED))
            {
                // Downgrade to the older implementation
                impl = remove_disp_ex_flag_posix_semantics;
                filesystem::detail::atomic_store_relaxed(g_remove_impl_type, impl);
            }
            else
            {
                break;
            }
        }
        BOOST_FALLTHROUGH;

    case remove_disp_ex_flag_posix_semantics:
        {
            file_disposition_info_ex info;
            info.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
            BOOL res = set_file_information_by_handle(handle, file_disposition_info_ex_class, &info, sizeof(info));
            if (BOOST_LIKELY(!!res))
            {
                err = 0u;
                break;
            }

            err = ::GetLastError();
            if (err == ERROR_ACCESS_DENIED)
            {
                // Check if the file is read-only and reset the attribute
                file_basic_info basic_info;
                res = get_file_information_by_handle_ex(handle, file_basic_info_class, &basic_info, sizeof(basic_info));
                if (BOOST_UNLIKELY(!res || (basic_info.FileAttributes & FILE_ATTRIBUTE_READONLY) == 0))
                    break; // return ERROR_ACCESS_DENIED

                basic_info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;

                res = set_file_information_by_handle(handle, file_basic_info_class, &basic_info, sizeof(basic_info));
                if (BOOST_UNLIKELY(!res))
                {
                    err = ::GetLastError();
                    break;
                }

                // Try to set the flag again
                res = set_file_information_by_handle(handle, file_disposition_info_ex_class, &info, sizeof(info));
                if (BOOST_LIKELY(!!res))
                {
                    err = 0u;
                    break;
                }

                err = ::GetLastError();

                // Try to restore the read-only flag
                basic_info.FileAttributes |= FILE_ATTRIBUTE_READONLY;
                set_file_information_by_handle(handle, file_basic_info_class, &basic_info, sizeof(basic_info));

                break;
            }
            else if (BOOST_UNLIKELY(err == ERROR_INVALID_PARAMETER || err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED || err == ERROR_CALL_NOT_IMPLEMENTED))
            {
                // Downgrade to the older implementation
                impl = remove_disp;
                filesystem::detail::atomic_store_relaxed(g_remove_impl_type, impl);
            }
            else
            {
                break;
            }
        }
        BOOST_FALLTHROUGH;

    default:
        {
            file_disposition_info info;
            info.DeleteFile = true;
            BOOL res = set_file_information_by_handle(handle, file_disposition_info_class, &info, sizeof(info));
            if (BOOST_LIKELY(!!res))
            {
                err = 0u;
                break;
            }

            err = ::GetLastError();
            if (err == ERROR_ACCESS_DENIED)
            {
                // Check if the file is read-only and reset the attribute
                file_basic_info basic_info;
                res = get_file_information_by_handle_ex(handle, file_basic_info_class, &basic_info, sizeof(basic_info));
                if (BOOST_UNLIKELY(!res || (basic_info.FileAttributes & FILE_ATTRIBUTE_READONLY) == 0))
                    break; // return ERROR_ACCESS_DENIED

                basic_info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;

                res = set_file_information_by_handle(handle, file_basic_info_class, &basic_info, sizeof(basic_info));
                if (BOOST_UNLIKELY(!res))
                {
                    err = ::GetLastError();
                    break;
                }

                // Try to set the flag again
                res = set_file_information_by_handle(handle, file_disposition_info_class, &info, sizeof(info));
                if (BOOST_LIKELY(!!res))
                {
                    err = 0u;
                    break;
                }

                err = ::GetLastError();

                // Try to restore the read-only flag
                basic_info.FileAttributes |= FILE_ATTRIBUTE_READONLY;
                set_file_information_by_handle(handle, file_basic_info_class, &basic_info, sizeof(basic_info));
            }

            break;
        }
    }

    return err;
}

//! remove() implementation for Windows Vista and newer
inline bool remove_nt6_impl(path const& p, remove_impl_type impl, error_code* ec)
{
    unique_handle h(create_file_handle(
        p,
        DELETE | FILE_READ_ATTRIBUTES | FILE_READ_EA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
    DWORD err = 0u;
    if (BOOST_UNLIKELY(!h))
    {
        err = ::GetLastError();

    return_error:
        if (!not_found_error(err))
            emit_error(err, p, ec, "boost::filesystem::remove");

        return false;
    }

    err = fs::detail::remove_nt6_by_handle(h.get(), impl);
    if (BOOST_UNLIKELY(err != 0u))
        goto return_error;

    return true;
}

//! remove() implementation
inline bool remove_impl(path const& p, error_code* ec)
{
    remove_impl_type impl = fs::detail::atomic_load_relaxed(g_remove_impl_type);
    if (BOOST_LIKELY(impl != remove_nt5))
    {
        return fs::detail::remove_nt6_impl(p, impl, ec);
    }
    else
    {
        const DWORD attrs = ::GetFileAttributesW(p.c_str());
        if (BOOST_UNLIKELY(attrs == INVALID_FILE_ATTRIBUTES))
        {
            DWORD err = ::GetLastError();
            if (!not_found_error(err))
                emit_error(err, p, ec, "boost::filesystem::remove");

            return false;
        }

        return fs::detail::remove_nt5_impl(p, attrs, ec);
    }
}

//! remove_all() by handle implementation for Windows Vista and newer
uintmax_t remove_all_nt6_by_handle(HANDLE h, path const& p, error_code* ec)
{
    error_code local_ec;
    fs::file_status st(fs::detail::status_by_handle(h, p, &local_ec));
    if (BOOST_UNLIKELY(st.type() == fs::status_error))
    {
        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::remove_all", p, local_ec));

        *ec = local_ec;
        return static_cast< uintmax_t >(-1);
    }

    uintmax_t count = 0u;
    if (st.type() == fs::directory_file)
    {
        local_ec.clear();

        fs::directory_iterator itr;
        directory_iterator_params params;
        params.dir_handle = h;
        params.close_handle = false; // the caller will close the handle
        fs::detail::directory_iterator_construct(itr, p, directory_options::_detail_no_follow, &params, &local_ec);
        if (BOOST_UNLIKELY(!!local_ec))
        {
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::remove_all", p, local_ec));

            *ec = local_ec;
            return static_cast< uintmax_t >(-1);
        }

        NtCreateFile_t* nt_create_file = filesystem::detail::atomic_load_relaxed(nt_create_file_api);
        const fs::directory_iterator end_dit;
        while (itr != end_dit)
        {
            fs::path nested_path(itr->path());
            unique_handle hh;
            if (BOOST_LIKELY(nt_create_file != nullptr))
            {
                // Note: WinAPI methods like CreateFileW implicitly request SYNCHRONIZE access but NtCreateFile doesn't.
                // Without SYNCHRONIZE access querying file attributes via GetFileInformationByHandleEx fails with ERROR_ACCESS_DENIED.
                boost::winapi::NTSTATUS_ status = nt_create_file_handle_at
                (
                    hh,
                    h,
                    path_algorithms::filename_v4(nested_path),
                    0u, // FileAttributes
                    FILE_LIST_DIRECTORY | DELETE | FILE_READ_ATTRIBUTES | FILE_READ_EA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    FILE_OPEN,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_REPARSE_POINT
                );

                if (!NT_SUCCESS(status))
                {
                    if (not_found_ntstatus(status))
                        goto next_entry;

                    DWORD err = translate_ntstatus(status);
                    emit_error(err, nested_path, ec, "boost::filesystem::remove_all");
                    return static_cast< uintmax_t >(-1);
                }
            }
            else
            {
                hh = create_file_handle(
                    nested_path,
                    FILE_LIST_DIRECTORY | DELETE | FILE_READ_ATTRIBUTES | FILE_READ_EA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);

                if (BOOST_UNLIKELY(!hh))
                {
                    DWORD err = ::GetLastError();
                    if (not_found_error(err))
                        goto next_entry;

                    emit_error(err, nested_path, ec, "boost::filesystem::remove_all");
                    return static_cast< uintmax_t >(-1);
                }
            }

            count += fs::detail::remove_all_nt6_by_handle(hh.get(), nested_path, ec);
            if (ec && *ec)
                return static_cast< uintmax_t >(-1);

        next_entry:
            fs::detail::directory_iterator_increment(itr, ec);
            if (ec && *ec)
                return static_cast< uintmax_t >(-1);
        }
    }

    DWORD err = fs::detail::remove_nt6_by_handle(h, fs::detail::atomic_load_relaxed(g_remove_impl_type));
    if (BOOST_UNLIKELY(err != 0u))
    {
        emit_error(err, p, ec, "boost::filesystem::remove_all");
        return static_cast< uintmax_t >(-1);
    }

    ++count;
    return count;
}

//! remove_all() implementation for Windows XP and older
uintmax_t remove_all_nt5_impl(path const& p, error_code* ec)
{
    error_code dit_create_ec;
    for (unsigned int attempt = 0u; attempt < remove_all_directory_replaced_retry_count; ++attempt)
    {
        const DWORD attrs = ::GetFileAttributesW(p.c_str());
        if (BOOST_UNLIKELY(attrs == INVALID_FILE_ATTRIBUTES))
        {
            DWORD err = ::GetLastError();
            if (not_found_error(err))
                return 0u;

            emit_error(err, p, ec, "boost::filesystem::remove_all");
            return static_cast< uintmax_t >(-1);
        }

        // Recurse into directories, but not into junctions or directory symlinks
        const bool recurse = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        uintmax_t count = 0u;
        if (recurse)
        {
            fs::directory_iterator itr;
            fs::detail::directory_iterator_construct(itr, p, directory_options::_detail_no_follow, nullptr, &dit_create_ec);
            if (BOOST_UNLIKELY(!!dit_create_ec))
            {
                if (dit_create_ec == make_error_condition(system::errc::not_a_directory) ||
                    dit_create_ec == make_error_condition(system::errc::too_many_symbolic_link_levels))
                {
                    continue;
                }

                if (!ec)
                    BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::remove_all", p, dit_create_ec));

                *ec = dit_create_ec;
                return static_cast< uintmax_t >(-1);
            }

            const fs::directory_iterator end_dit;
            while (itr != end_dit)
            {
                count += fs::detail::remove_all_nt5_impl(itr->path(), ec);
                if (ec && *ec)
                    return static_cast< uintmax_t >(-1);

                fs::detail::directory_iterator_increment(itr, ec);
                if (ec && *ec)
                    return static_cast< uintmax_t >(-1);
            }
        }

        bool removed = fs::detail::remove_nt5_impl(p, attrs, ec);
        if (ec && *ec)
            return static_cast< uintmax_t >(-1);

        count += removed;
        return count;
    }

    if (!ec)
        BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::remove_all: path cannot be opened as a directory", p, dit_create_ec));

    *ec = dit_create_ec;
    return static_cast< uintmax_t >(-1);
}

//! remove_all() implementation
inline uintmax_t remove_all_impl(path const& p, error_code* ec)
{
    remove_impl_type impl = fs::detail::atomic_load_relaxed(g_remove_impl_type);
    if (BOOST_LIKELY(impl != remove_nt5))
    {
        unique_handle h(create_file_handle(
            p,
            FILE_LIST_DIRECTORY | DELETE | FILE_READ_ATTRIBUTES | FILE_READ_EA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));

        if (BOOST_UNLIKELY(!h))
        {
            DWORD err = ::GetLastError();
            if (not_found_error(err))
                return 0u;

            emit_error(err, p, ec, "boost::filesystem::remove_all");
            return static_cast< uintmax_t >(-1);
        }

        return fs::detail::remove_all_nt6_by_handle(h.get(), p, ec);
    }

    return fs::detail::remove_all_nt5_impl(p, ec);
}

inline BOOL resize_file_impl(const wchar_t* p, uintmax_t size)
{
    unique_handle h(CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER sz;
    sz.QuadPart = size;
    return !!h && ::SetFilePointerEx(h.get(), sz, 0, FILE_BEGIN) && ::SetEndOfFile(h.get());
}

//! Converts NT path to a Win32 path
inline path convert_nt_path_to_win32_path(const wchar_t* nt_path, std::size_t size)
{
    // https://googleprojectzero.blogspot.com/2016/02/the-definitive-guide-on-win32-to-nt.html
    // https://stackoverflow.com/questions/23041983/path-prefixes-and
    //
    // NT paths can be used to identify practically any named objects, devices, files, local and remote shares, etc.
    // The path starts with a leading backslash and consists of one or more path elements separated with backslashes.
    // The set of characters allowed in NT path elements is significantly larger than that of Win32 paths - basically,
    // any character except the backslash is allowed. Path elements are case-insensitive.
    //
    // NT paths that start with the "\??\" prefix are used to indicate the current user's session namespace. The prefix
    // indicates to the NT object manager to lookup the object relative to "\Sessions\0\DosDevices\[Logon Authentication ID]".
    //
    // There is also a special "\Global??\" prefix that refers to the system logon. User's session directory shadows
    // the system logon directory, so that when the referenced object is not found in the user's namespace,
    // system logon is looked up instead.
    //
    // There is a symlink "Global" in the user's session namespace that refers to the global namespace, so "\??\Global"
    // effectively resolves to "\Global??". This allows Win32 applications to directly refer to the system objects,
    // even if shadowed by the current user's logon object.
    //
    // NT paths can be used to reference not only local filesystems, but also devices and remote shares identifiable via
    // UNC paths. For this, there is a special "UNC" device (which is a symlink to "\Device\Mup") in the system logon
    // namespace, so "\??\UNC\host\share" (or "\??\Global\UNC\host\share", or "\Global??\UNC\host\share") is equivalent
    // to "\\host\share".
    //
    // NT paths are not universally accepted by Win32 applications and APIs. For example, Far supports paths starting
    // with "\??\" and "\??\Global\" but not with "\Global??\". As of Win10 21H1, File Explorer, cmd.exe and PowerShell
    // don't support any of these. Given this, and that NT paths have a different set of allowed characters from Win32 paths,
    // we should normally avoid exposing NT paths to users that expect Win32 paths.
    //
    // In Boost.Filesystem we only deal with NT paths that come from reparse points, such as symlinks and mount points,
    // including directory junctions. It was observed that reparse points created by junction.exe and mklink use the "\??\"
    // prefix for directory junctions and absolute symlink and unqualified relative path for relative symlinks.
    // Absolute paths are using drive letters for mounted drives (e.g. "\??\C:\directory"), although it is possible
    // to create a junction to an directory using a different way of identifying the filesystem (e.g.
    // "\??\Volume{00000000-0000-0000-0000-000000000000}\directory").
    // mklink does not support creating junctions pointing to a UNC path. junction.exe does create a junction that
    // uses a seemingly invalid syntax like "\??\\\host\share", i.e. it basically does not expect an UNC path. It is not known
    // if reparse points that refer to a UNC path are considered valid.
    // There are reparse points created as mount points for local and remote filsystems (for example, a cloud storage mounted
    // in the local filesystem). Such mount points have the form of "\??\Volume{00000000-0000-0000-0000-000000000000}\",
    // "\??\Harddisk0Partition1\" or "\??\HarddiskVolume1\".
    // Reparse points that refer directly to a global namespace (through "\??\Global\" or "\Global??\" prefixes) or
    // devices (e.g. "\Device\HarddiskVolume1") have not been observed so far.

    path win32_path;
    std::size_t pos = 0u;
    bool global_namespace = false;

    // Check for the "\??\" prefix
    if (size >= 4u &&
        nt_path[0] == path::preferred_separator &&
        nt_path[1] == questionmark &&
        nt_path[2] == questionmark &&
        nt_path[3] == path::preferred_separator)
    {
        pos = 4u;

        // Check "Global"
        if ((size - pos) >= 6u &&
            (nt_path[pos] == L'G' || nt_path[pos] == L'g') &&
            (nt_path[pos + 1] == L'l' || nt_path[pos + 1] == L'L') &&
            (nt_path[pos + 2] == L'o' || nt_path[pos + 2] == L'O') &&
            (nt_path[pos + 3] == L'b' || nt_path[pos + 3] == L'B') &&
            (nt_path[pos + 4] == L'a' || nt_path[pos + 4] == L'A') &&
            (nt_path[pos + 5] == L'l' || nt_path[pos + 5] == L'L'))
        {
            if ((size - pos) == 6u)
            {
                pos += 6u;
                global_namespace = true;
            }
            else if (detail::is_directory_separator(nt_path[pos + 6u]))
            {
                pos += 7u;
                global_namespace = true;
            }
        }
    }
    // Check for the "\Global??\" prefix
    else if (size >= 10u &&
        nt_path[0] == path::preferred_separator &&
        (nt_path[1] == L'G' || nt_path[1] == L'g') &&
        (nt_path[2] == L'l' || nt_path[2] == L'L') &&
        (nt_path[3] == L'o' || nt_path[3] == L'O') &&
        (nt_path[4] == L'b' || nt_path[4] == L'B') &&
        (nt_path[5] == L'a' || nt_path[5] == L'A') &&
        (nt_path[6] == L'l' || nt_path[6] == L'L') &&
        nt_path[7] == questionmark &&
        nt_path[8] == questionmark &&
        nt_path[9] == path::preferred_separator)
    {
        pos = 10u;
        global_namespace = true;
    }

    if (pos > 0u)
    {
        if ((size - pos) >= 2u &&
        (
            // Check if the following is a drive letter
            (
                nt_path[pos + 1u] == colon && detail::is_letter(nt_path[pos]) &&
                ((size - pos) == 2u || detail::is_directory_separator(nt_path[pos + 2u]))
            ) ||
            // Check for an "incorrect" syntax for UNC path junction points
            (
                detail::is_directory_separator(nt_path[pos]) && detail::is_directory_separator(nt_path[pos + 1u]) &&
                ((size - pos) == 2u || !detail::is_directory_separator(nt_path[pos + 2u]))
            )
        ))
        {
            // Strip the NT path prefix
            goto done;
        }

        static const wchar_t win32_path_prefix[4u] = { path::preferred_separator, path::preferred_separator, questionmark, path::preferred_separator };

        // Check for a UNC path
        if ((size - pos) >= 4u &&
            (nt_path[pos] == L'U' || nt_path[pos] == L'u') &&
            (nt_path[pos + 1] == L'N' || nt_path[pos + 1] == L'n') &&
            (nt_path[pos + 2] == L'C' || nt_path[pos + 2] == L'c') &&
            nt_path[pos + 3] == path::preferred_separator)
        {
            win32_path.assign(win32_path_prefix, win32_path_prefix + 2);
            pos += 4u;
            goto done;
        }

        // This is some other NT path, possibly a volume mount point. Replace the NT prefix with a Win32 filesystem prefix "\\?\".
        win32_path.assign(win32_path_prefix, win32_path_prefix + 4);
        if (global_namespace)
        {
            static const wchar_t win32_path_global_prefix[7u] = { L'G', L'l', L'o', L'b', L'a', L'l', path::preferred_separator };
            win32_path.concat(win32_path_global_prefix, win32_path_global_prefix + 7);
        }
    }

done:
    win32_path.concat(nt_path + pos, nt_path + size);
    return win32_path;
}

#endif // defined(BOOST_POSIX_API)

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
#ifdef BOOST_POSIX_API
    typedef struct stat struct_stat;
    return sizeof(struct_stat().st_size) > 4;
#else
    return true;
#endif
}

BOOST_FILESYSTEM_DECL
path absolute_v3(path const& p, path const& base, system::error_code* ec)
{
    if (ec)
        ec->clear();

    if (p.is_absolute())
        return p;

    //  recursively calling absolute is sub-optimal, but is sure and simple
    path abs_base = base;
    if (!base.is_absolute())
    {
        path cur_path = detail::current_path(ec);
        if (ec && *ec)
        {
        return_empty_path:
            return path();
        }

        if (BOOST_UNLIKELY(!cur_path.is_absolute()))
        {
            system::error_code local_ec = system::errc::make_error_code(system::errc::invalid_argument);
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::absolute", p, base, local_ec));

            *ec = local_ec;
            goto return_empty_path;
        }

        abs_base = detail::absolute_v3(base, cur_path, ec);
        if (ec && *ec)
            goto return_empty_path;
    }

    if (p.empty())
        return abs_base;

    path res;
    if (p.has_root_name())
        res = p.root_name();
    else
        res = abs_base.root_name();

    if (p.has_root_directory())
    {
        res.concat(p.root_directory());
    }
    else
    {
        res.concat(abs_base.root_directory());
        path_algorithms::append_v4(res, abs_base.relative_path());
    }

    path p_relative_path(p.relative_path());
    if (!p_relative_path.empty())
        path_algorithms::append_v4(res, p_relative_path);

    return res;
}

BOOST_FILESYSTEM_DECL
path absolute_v4(path const& p, path const& base, system::error_code* ec)
{
    if (ec)
        ec->clear();

    if (p.is_absolute())
        return p;

    path abs_base = base;
    if (!base.is_absolute())
    {
        path cur_path = detail::current_path(ec);
        if (ec && *ec)
        {
        return_empty_path:
            return path();
        }

        if (BOOST_UNLIKELY(!cur_path.is_absolute()))
        {
            system::error_code local_ec = system::errc::make_error_code(system::errc::invalid_argument);
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::absolute", p, base, local_ec));

            *ec = local_ec;
            goto return_empty_path;
        }

        abs_base = detail::absolute_v4(base, cur_path, ec);
        if (ec && *ec)
            goto return_empty_path;
    }

    path res;
    if (p.has_root_name())
        res = p.root_name();
    else
        res = abs_base.root_name();

    if (p.has_root_directory())
    {
        res.concat(p.root_directory());
    }
    else
    {
        res.concat(abs_base.root_directory());
        path_algorithms::append_v4(res, abs_base.relative_path());
    }

    path_algorithms::append_v4(res, p.relative_path());

    return res;
}

namespace {

inline path canonical_common(path& source, system::error_code* ec)
{
#if defined(BOOST_POSIX_API)

    system::error_code local_ec;
    file_status st(detail::status_impl(source, &local_ec));

    if (st.type() == fs::file_not_found)
    {
        local_ec = system::errc::make_error_code(system::errc::no_such_file_or_directory);
        goto fail_local_ec;
    }
    else if (local_ec)
    {
    fail_local_ec:
        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::canonical", source, local_ec));

        *ec = local_ec;

    return_empty_path:
        return path();
    }

    path root(source.root_path());
    path const& dot_p = dot_path();
    path const& dot_dot_p = dot_dot_path();
    unsigned int symlinks_allowed = symloop_max;
    path result;
    while (true)
    {
        for (path::iterator itr(source.begin()), end(source.end()); itr != end; path_algorithms::increment_v4(itr))
        {
            if (itr->empty())
                continue;
            if (path_algorithms::compare_v4(*itr, dot_p) == 0)
                continue;
            if (path_algorithms::compare_v4(*itr, dot_dot_p) == 0)
            {
                if (path_algorithms::compare_v4(result, root) != 0)
                    result.remove_filename_and_trailing_separators();
                continue;
            }

            if (itr->size() == 1u && detail::is_directory_separator(itr->native()[0]))
            {
                // Convert generic separator returned by the iterator for the root directory to
                // the preferred separator. This is important on Windows, as in some cases,
                // like paths for network shares and cloud storage mount points GetFileAttributesW
                // will return "file not found" if the path contains forward slashes.
                result += path::preferred_separator;
                // We don't need to check for a symlink after adding a separator.
                continue;
            }

            path_algorithms::append_v4(result, *itr);

            st = detail::symlink_status_impl(result, ec);
            if (ec && *ec)
                goto return_empty_path;

            if (is_symlink(st))
            {
                if (symlinks_allowed == 0)
                {
                    local_ec = system::errc::make_error_code(system::errc::too_many_symbolic_link_levels);
                    goto fail_local_ec;
                }

                --symlinks_allowed;

                path link(detail::read_symlink(result, ec));
                if (ec && *ec)
                    goto return_empty_path;
                result.remove_filename_and_trailing_separators();

                if (link.is_absolute())
                {
                    for (path_algorithms::increment_v4(itr); itr != end; path_algorithms::increment_v4(itr))
                    {
                        if (path_algorithms::compare_v4(*itr, dot_p) != 0)
                            path_algorithms::append_v4(link, *itr);
                    }
                    source = std::move(link);
                    root = source.root_path();
                }
                else // link is relative
                {
                    link.remove_trailing_separator();
                    if (path_algorithms::compare_v4(link, dot_p) == 0)
                        continue;

                    path new_source(result);
                    path_algorithms::append_v4(new_source, link);
                    for (path_algorithms::increment_v4(itr); itr != end; path_algorithms::increment_v4(itr))
                    {
                        if (path_algorithms::compare_v4(*itr, dot_p) != 0)
                            path_algorithms::append_v4(new_source, *itr);
                    }
                    source = std::move(new_source);
                }

                // symlink causes scan to be restarted
                goto restart_scan;
            }
        }

        break;

    restart_scan:
        result.clear();
    }

    BOOST_ASSERT_MSG(result.is_absolute(), "canonical() implementation error; please report");
    return result;

#else // defined(BOOST_POSIX_API)

    unique_handle h(create_file_handle(
        source.c_str(),
        FILE_READ_ATTRIBUTES | FILE_READ_EA,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, // lpSecurityAttributes
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));

    DWORD err;
    if (!h)
    {
        err = ::GetLastError();

    fail_err:
        emit_error(err, source, ec, "boost::filesystem::canonical");
        return path();
    }

    WCHAR path_small_buf[small_path_size];
    std::unique_ptr< WCHAR[] > path_large_buf;
    WCHAR* path_buf = path_small_buf;
    DWORD path_buf_size = sizeof(path_small_buf) / sizeof(*path_small_buf);
    DWORD flags = FILE_NAME_NORMALIZED;
    DWORD path_len;

    while (true)
    {
        path_len = ::GetFinalPathNameByHandleW(h.get(), path_buf, path_buf_size, flags);
        if (path_len > 0)
        {
            if (path_len < path_buf_size)
                break;

            // The buffer is not large enough, path_len is the required buffer size, including the terminating zero
            path_large_buf.reset(new WCHAR[path_len]);
            path_buf = path_large_buf.get();
            path_buf_size = path_len;
        }
        else
        {
            err = ::GetLastError();
            if (BOOST_UNLIKELY(err == ERROR_PATH_NOT_FOUND && (flags & VOLUME_NAME_NT) == 0u))
            {
                // Drive letter does not exist for the file, obtain an NT path for it
                flags |= VOLUME_NAME_NT;
                continue;
            }

            goto fail_err;
        }
    }

    path result;
    if ((flags & VOLUME_NAME_NT) == 0u)
    {
        // If the input path did not contain a long path prefix, convert
        // "\\?\X:" to "X:" and "\\?\UNC\" to "\\". Otherwise, preserve the prefix.
        const path::value_type* source_str = source.c_str();
        if (source.size() < 4u ||
            source_str[0] != path::preferred_separator ||
            source_str[1] != path::preferred_separator ||
            source_str[2] != questionmark ||
            source_str[3] != path::preferred_separator)
        {
            if (path_len >= 6u &&
                path_buf[0] == path::preferred_separator &&
                path_buf[1] == path::preferred_separator &&
                path_buf[2] == questionmark &&
                path_buf[3] == path::preferred_separator)
            {
                if (path_buf[5] == colon && detail::is_letter(path_buf[4]))
                {
                    path_buf += 4;
                    path_len -= 4u;
                }
                else if (path_len >= 8u &&
                    (path_buf[4] == L'U' || path_buf[4] == L'u') &&
                    (path_buf[5] == L'N' || path_buf[5] == L'n') &&
                    (path_buf[6] == L'C' || path_buf[6] == L'c') &&
                    path_buf[7] == path::preferred_separator)
                {
                    path_buf += 6;
                    path_len -= 6u;
                    path_buf[0] = path::preferred_separator;
                }
            }
        }

        result.assign(path_buf, path_buf + path_len);
    }
    else
    {
        // Convert NT path to a Win32 path
        result.assign(L"\\\\?\\GLOBALROOT");
        result.concat(path_buf, path_buf + path_len);
    }

    BOOST_ASSERT_MSG(result.is_absolute(), "canonical() implementation error; please report");
    return result;

#endif // defined(BOOST_POSIX_API)
}

} // unnamed namespace

BOOST_FILESYSTEM_DECL
path canonical_v3(path const& p, path const& base, system::error_code* ec)
{
    path source(detail::absolute_v3(p, base, ec));
    if (ec && *ec)
        return path();

    return detail::canonical_common(source, ec);
}

BOOST_FILESYSTEM_DECL
path canonical_v4(path const& p, path const& base, system::error_code* ec)
{
    path source(detail::absolute_v4(p, base, ec));
    if (ec && *ec)
        return path();

    return detail::canonical_common(source, ec);
}

BOOST_FILESYSTEM_DECL
void copy(path const& from, path const& to, copy_options options, system::error_code* ec)
{
    BOOST_ASSERT((((options & copy_options::overwrite_existing) != copy_options::none) +
        ((options & copy_options::skip_existing) != copy_options::none) +
        ((options & copy_options::update_existing) != copy_options::none)) <= 1);

    BOOST_ASSERT((((options & copy_options::copy_symlinks) != copy_options::none) +
        ((options & copy_options::skip_symlinks) != copy_options::none)) <= 1);

    BOOST_ASSERT((((options & copy_options::directories_only) != copy_options::none) +
        ((options & copy_options::create_symlinks) != copy_options::none) +
        ((options & copy_options::create_hard_links) != copy_options::none)) <= 1);

    if (ec)
        ec->clear();

    file_status from_stat;
    if ((options & (copy_options::copy_symlinks | copy_options::skip_symlinks | copy_options::create_symlinks)) != copy_options::none)
    {
        from_stat = detail::symlink_status_impl(from, ec);
    }
    else
    {
        from_stat = detail::status_impl(from, ec);
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
        if ((options & copy_options::skip_symlinks) != copy_options::none)
            return;

        if ((options & copy_options::copy_symlinks) == copy_options::none)
            goto fail;

        detail::copy_symlink(from, to, ec);
    }
    else if (is_regular_file(from_stat))
    {
        if ((options & copy_options::directories_only) != copy_options::none)
            return;

        if ((options & copy_options::create_symlinks) != copy_options::none)
        {
            const path* pfrom = &from;
            path relative_from;
            if (!from.is_absolute())
            {
                // Try to generate a relative path from the target location to the original file
                path cur_dir = detail::current_path(ec);
                if (ec && *ec)
                    return;
                path abs_from = detail::absolute_v4(from.parent_path(), cur_dir, ec);
                if (ec && *ec)
                    return;
                path abs_to = to.parent_path();
                if (!abs_to.is_absolute())
                {
                    abs_to = detail::absolute_v4(abs_to, cur_dir, ec);
                    if (ec && *ec)
                        return;
                }
                relative_from = detail::relative(abs_from, abs_to, ec);
                if (ec && *ec)
                    return;
                if (path_algorithms::compare_v4(relative_from, dot_path()) != 0)
                    path_algorithms::append_v4(relative_from, path_algorithms::filename_v4(from));
                else
                    relative_from = path_algorithms::filename_v4(from);
                pfrom = &relative_from;
            }
            detail::create_symlink(*pfrom, to, ec);
            return;
        }

        if ((options & copy_options::create_hard_links) != copy_options::none)
        {
            detail::create_hard_link(from, to, ec);
            return;
        }

        error_code local_ec;
        file_status to_stat;
        if ((options & (copy_options::skip_symlinks | copy_options::create_symlinks)) != copy_options::none)
        {
            to_stat = detail::symlink_status_impl(to, &local_ec);
        }
        else
        {
            to_stat = detail::status_impl(to, &local_ec);
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
        {
            path target(to);
            path_algorithms::append_v4(target, path_algorithms::filename_v4(from));
            detail::copy_file(from, target, options, ec);
        }
        else
            detail::copy_file(from, to, options, ec);
    }
    else if (is_directory(from_stat))
    {
        error_code local_ec;
        if ((options & copy_options::create_symlinks) != copy_options::none)
        {
            local_ec = make_error_code(system::errc::is_a_directory);
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::copy", from, to, local_ec));
            *ec = local_ec;
            return;
        }

        file_status to_stat;
        if ((options & (copy_options::skip_symlinks | copy_options::create_symlinks)) != copy_options::none)
        {
            to_stat = detail::symlink_status_impl(to, &local_ec);
        }
        else
        {
            to_stat = detail::status_impl(to, &local_ec);
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

        if ((options & copy_options::recursive) != copy_options::none || options == copy_options::none)
        {
            fs::directory_iterator itr;
            detail::directory_iterator_construct(itr, from, directory_options::none, nullptr, ec);
            if (ec && *ec)
                return;

            const fs::directory_iterator end_dit;
            while (itr != end_dit)
            {
                path const& p = itr->path();
                {
                    path target(to);
                    path_algorithms::append_v4(target, path_algorithms::filename_v4(p));
                    // Set _detail_recursing flag so that we don't recurse more than for one level deeper into the directory if options are copy_options::none
                    detail::copy(p, target, options | copy_options::_detail_recursing, ec);
                }
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
bool copy_file(path const& from, path const& to, copy_options options, error_code* ec)
{
    BOOST_ASSERT((((options & copy_options::overwrite_existing) != copy_options::none) +
        ((options & copy_options::skip_existing) != copy_options::none) +
        ((options & copy_options::update_existing) != copy_options::none)) <= 1);

    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)

    int err = 0;

    // Note: Declare fd wrappers here so that errno is not clobbered by close() that may be called in fd wrapper destructors
    boost::scope::unique_fd infile, outfile;

    while (true)
    {
        infile.reset(::open(from.c_str(), O_RDONLY | O_CLOEXEC));
        if (BOOST_UNLIKELY(!infile))
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

#if defined(BOOST_FILESYSTEM_USE_STATX)
    unsigned int statx_data_mask = STATX_TYPE | STATX_MODE | STATX_INO | STATX_SIZE;
    if ((options & copy_options::update_existing) != copy_options::none)
        statx_data_mask |= STATX_MTIME;

    struct ::statx from_stat;
    if (BOOST_UNLIKELY(invoke_statx(infile.get(), "", AT_EMPTY_PATH | AT_NO_AUTOMOUNT, statx_data_mask, &from_stat) < 0))
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
    if (BOOST_UNLIKELY(::fstat(infile.get(), &from_stat) != 0))
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

    mode_t to_mode = from_mode & fs::perms_mask;
#if !defined(BOOST_FILESYSTEM_USE_WASI)
    // Enable writing for the newly created files. Having write permission set is important e.g. for NFS,
    // which checks the file permission on the server, even if the client's file descriptor supports writing.
    to_mode |= S_IWUSR;
#endif
    int oflag = O_WRONLY | O_CLOEXEC;

    if ((options & copy_options::update_existing) != copy_options::none)
    {
        // Try opening the existing file without truncation to test the modification time later
        while (true)
        {
            outfile.reset(::open(to.c_str(), oflag, to_mode));
            if (!outfile)
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
        if (((options & copy_options::overwrite_existing) == copy_options::none ||
             (options & copy_options::skip_existing) != copy_options::none) &&
            (options & copy_options::update_existing) == copy_options::none)
        {
            oflag |= O_EXCL;
        }

        while (true)
        {
            outfile.reset(::open(to.c_str(), oflag, to_mode));
            if (!outfile)
            {
                err = errno;
                if (err == EINTR)
                    continue;

                if (err == EEXIST && (options & copy_options::skip_existing) != copy_options::none)
                    return false;

                goto fail;
            }

            break;
        }
    }

#if defined(BOOST_FILESYSTEM_USE_STATX)
    statx_data_mask = STATX_TYPE | STATX_MODE | STATX_INO;
    if ((oflag & O_TRUNC) == 0)
    {
        // O_TRUNC is not set if copy_options::update_existing is set and an existing file was opened.
        statx_data_mask |= STATX_MTIME;
    }

    struct ::statx to_stat;
    if (BOOST_UNLIKELY(invoke_statx(outfile.get(), "", AT_EMPTY_PATH | AT_NO_AUTOMOUNT, statx_data_mask, &to_stat) < 0))
        goto fail_errno;

    if (BOOST_UNLIKELY((to_stat.stx_mask & statx_data_mask) != statx_data_mask))
    {
        err = ENOSYS;
        goto fail;
    }
#else
    struct ::stat to_stat;
    if (BOOST_UNLIKELY(::fstat(outfile.get(), &to_stat) != 0))
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
#if defined(BOOST_FILESYSTEM_USE_STATX)
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

        if (BOOST_UNLIKELY(::ftruncate(outfile.get(), 0) != 0))
            goto fail_errno;
    }

    // Note: Use block size of the target file since it is most important for writing performance.
    err = filesystem::detail::atomic_load_relaxed(filesystem::detail::copy_file_data)(infile.get(), outfile.get(), get_size(from_stat), get_blksize(to_stat));
    if (BOOST_UNLIKELY(err != 0))
        goto fail; // err already contains the error code

#if !defined(BOOST_FILESYSTEM_USE_WASI)
    // If we created a new file with an explicitly added S_IWUSR permission,
    // we may need to update its mode bits to match the source file.
    if ((to_mode & fs::perms_mask) != (from_mode & fs::perms_mask))
    {
        if (BOOST_UNLIKELY(::fchmod(outfile.get(), (from_mode & fs::perms_mask)) != 0 &&
            (options & copy_options::ignore_attribute_errors) == copy_options::none))
        {
            goto fail_errno;
        }
    }
#endif

    if ((options & (copy_options::synchronize_data | copy_options::synchronize)) != copy_options::none)
    {
        if ((options & copy_options::synchronize) != copy_options::none)
            err = full_sync(outfile.get());
        else
            err = data_sync(outfile.get());

        if (BOOST_UNLIKELY(err != 0))
            goto fail;
    }

    // We have to explicitly close the output file descriptor in order to handle a possible error returned from it. The error may indicate
    // a failure of a prior write operation.
    err = close_fd(outfile.get());
    outfile.release();
    if (BOOST_UNLIKELY(err < 0))
    {
        err = errno;
        // EINPROGRESS is an allowed error code in future POSIX revisions, according to https://www.austingroupbugs.net/view.php?id=529#c1200.
        if (err != EINTR && err != EINPROGRESS)
            goto fail;
    }

    return true;

#else // defined(BOOST_POSIX_API)

    DWORD copy_flags = 0u;
    if ((options & copy_options::overwrite_existing) == copy_options::none ||
        (options & copy_options::skip_existing) != copy_options::none)
    {
        copy_flags |= COPY_FILE_FAIL_IF_EXISTS;
    }

    if ((options & copy_options::update_existing) != copy_options::none)
    {
        // Create unique_handle wrappers here so that CloseHandle calls don't clobber error code returned by GetLastError
        unique_handle hw_from, hw_to;

        // See the comment in last_write_time regarding access rights used here for GetFileTime.
        hw_from = create_file_handle(
            from.c_str(),
            FILE_READ_ATTRIBUTES | FILE_READ_EA,
            FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS);

        FILETIME lwt_from;
        if (!hw_from)
        {
        fail_last_error:
            DWORD err = ::GetLastError();
            emit_error(err, from, to, ec, "boost::filesystem::copy_file");
            return false;
        }

        if (!::GetFileTime(hw_from.get(), nullptr, nullptr, &lwt_from))
            goto fail_last_error;

        hw_to = create_file_handle(
            to.c_str(),
            FILE_READ_ATTRIBUTES | FILE_READ_EA,
            FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS);

        if (!!hw_to)
        {
            FILETIME lwt_to;
            if (!::GetFileTime(hw_to.get(), nullptr, nullptr, &lwt_to))
                goto fail_last_error;

            ULONGLONG tfrom = (static_cast< ULONGLONG >(lwt_from.dwHighDateTime) << 32) | static_cast< ULONGLONG >(lwt_from.dwLowDateTime);
            ULONGLONG tto = (static_cast< ULONGLONG >(lwt_to.dwHighDateTime) << 32) | static_cast< ULONGLONG >(lwt_to.dwLowDateTime);
            if (tfrom <= tto)
                return false;
        }

        copy_flags &= ~static_cast< DWORD >(COPY_FILE_FAIL_IF_EXISTS);
    }

    struct callback_context
    {
        DWORD flush_error;
    };

    struct local
    {
        //! Callback that is called to report progress of \c CopyFileExW
        static DWORD WINAPI on_copy_file_progress(
            LARGE_INTEGER total_file_size,
            LARGE_INTEGER total_bytes_transferred,
            LARGE_INTEGER stream_size,
            LARGE_INTEGER stream_bytes_transferred,
            DWORD stream_number,
            DWORD callback_reason,
            HANDLE from_handle,
            HANDLE to_handle,
            LPVOID ctx)
        {
            // For each stream, CopyFileExW will open a separate pair of file handles, so we need to flush each stream separately.
            if (stream_bytes_transferred.QuadPart == stream_size.QuadPart)
            {
                BOOL res = ::FlushFileBuffers(to_handle);
                if (BOOST_UNLIKELY(!res))
                {
                    callback_context* context = static_cast< callback_context* >(ctx);
                    if (BOOST_LIKELY(context->flush_error == 0u))
                        context->flush_error = ::GetLastError();
                }
            }

            return PROGRESS_CONTINUE;
        }
    };

    callback_context cb_context = {};
    LPPROGRESS_ROUTINE cb = nullptr;
    LPVOID cb_ctx = nullptr;

    if ((options & (copy_options::synchronize_data | copy_options::synchronize)) != copy_options::none)
    {
        cb = &local::on_copy_file_progress;
        cb_ctx = &cb_context;
    }

    BOOL cancelled = FALSE;
    BOOL res = ::CopyFileExW(from.c_str(), to.c_str(), cb, cb_ctx, &cancelled, copy_flags);
    DWORD err;
    if (BOOST_UNLIKELY(!res))
    {
        err = ::GetLastError();
        if ((err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) && (options & copy_options::skip_existing) != copy_options::none)
            return false;

    copy_failed:
        emit_error(err, from, to, ec, "boost::filesystem::copy_file");
        return false;
    }

    if (BOOST_UNLIKELY(cb_context.flush_error != 0u))
    {
        err = cb_context.flush_error;
        goto copy_failed;
    }

    return true;

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
void copy_symlink(path const& existing_symlink, path const& new_symlink, system::error_code* ec)
{
    path p(read_symlink(existing_symlink, ec));
    if (ec && *ec)
        return;
    create_symlink(p, new_symlink, ec);
}

BOOST_FILESYSTEM_DECL
bool create_directories(path const& p, system::error_code* ec)
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

    if (ec)
        ec->clear();

    path::const_iterator e(p.end()), it(e);
    path parent(p);
    path const& dot_p = dot_path();
    path const& dot_dot_p = dot_dot_path();
    error_code local_ec;

    // Find the initial part of the path that exists
    for (path fname = path_algorithms::filename_v4(parent); parent.has_relative_path(); fname = path_algorithms::filename_v4(parent))
    {
        if (!fname.empty() && path_algorithms::compare_v4(fname, dot_p) != 0 && path_algorithms::compare_v4(fname, dot_dot_p) != 0)
        {
            file_status existing_status = detail::status_impl(parent, &local_ec);

            if (existing_status.type() == directory_file)
            {
                break;
            }
            else if (BOOST_UNLIKELY(existing_status.type() == status_error))
            {
                if (!ec)
                    BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::create_directories", p, parent, local_ec));
                *ec = local_ec;
                return false;
            }
        }

        path_algorithms::decrement_v4(it);
        parent.remove_filename_and_trailing_separators();
    }

    // Create missing directories
    bool created = false;
    for (; it != e; path_algorithms::increment_v4(it))
    {
        path const& fname = *it;
        path_algorithms::append_v4(parent, fname);
        if (!fname.empty() && path_algorithms::compare_v4(fname, dot_p) != 0 && path_algorithms::compare_v4(fname, dot_dot_p) != 0)
        {
            created = detail::create_directory(parent, nullptr, &local_ec);
            if (BOOST_UNLIKELY(!!local_ec))
            {
                if (!ec)
                    BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::create_directories", p, parent, local_ec));
                *ec = local_ec;
                return false;
            }
        }
    }

    return created;
}

BOOST_FILESYSTEM_DECL
bool create_directory(path const& p, const path* existing, error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)

    mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO;
    if (existing)
    {
#if defined(BOOST_FILESYSTEM_USE_STATX)
        struct ::statx existing_stat;
        if (BOOST_UNLIKELY(invoke_statx(AT_FDCWD, existing->c_str(), AT_NO_AUTOMOUNT, STATX_TYPE | STATX_MODE, &existing_stat) < 0))
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
        res = ::CreateDirectoryExW(existing->c_str(), p.c_str(), nullptr);
    else
        res = ::CreateDirectoryW(p.c_str(), nullptr);

    if (res)
        return true;

#endif // defined(BOOST_POSIX_API)

    //  attempt to create directory failed
    err_t errval = BOOST_ERRNO; // save reason for failure
    error_code dummy;

    if (is_directory(p, dummy))
        return false;

    //  attempt to create directory failed && it doesn't already exist
    emit_error(errval, p, ec, "boost::filesystem::create_directory");
    return false;
}

BOOST_FILESYSTEM_DECL
void create_directory_symlink(path const& to, path const& from, system::error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)
    int err = ::symlink(to.c_str(), from.c_str());
    if (BOOST_UNLIKELY(err < 0))
    {
        err = errno;
        emit_error(err, to, from, ec, "boost::filesystem::create_directory_symlink");
    }
#else
    // see if actually supported by Windows runtime dll
    if (!create_symbolic_link_api)
    {
        emit_error(BOOST_ERROR_NOT_SUPPORTED, to, from, ec, "boost::filesystem::create_directory_symlink");
        return;
    }

    if (!create_symbolic_link_api(from.c_str(), to.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
    {
        emit_error(BOOST_ERRNO, to, from, ec, "boost::filesystem::create_directory_symlink");
    }
#endif
}

BOOST_FILESYSTEM_DECL
void create_hard_link(path const& to, path const& from, error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)
    int err = ::link(to.c_str(), from.c_str());
    if (BOOST_UNLIKELY(err < 0))
    {
        err = errno;
        emit_error(err, to, from, ec, "boost::filesystem::create_hard_link");
    }
#else
    // see if actually supported by Windows runtime dll
    CreateHardLinkW_t* chl_api = filesystem::detail::atomic_load_relaxed(create_hard_link_api);
    if (BOOST_UNLIKELY(!chl_api))
    {
        emit_error(BOOST_ERROR_NOT_SUPPORTED, to, from, ec, "boost::filesystem::create_hard_link");
        return;
    }

    if (BOOST_UNLIKELY(!chl_api(from.c_str(), to.c_str(), nullptr)))
    {
        emit_error(BOOST_ERRNO, to, from, ec, "boost::filesystem::create_hard_link");
    }
#endif
}

BOOST_FILESYSTEM_DECL
void create_symlink(path const& to, path const& from, error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)
    int err = ::symlink(to.c_str(), from.c_str());
    if (BOOST_UNLIKELY(err < 0))
    {
        err = errno;
        emit_error(err, to, from, ec, "boost::filesystem::create_symlink");
    }
#else
    // see if actually supported by Windows runtime dll
    CreateSymbolicLinkW_t* csl_api = filesystem::detail::atomic_load_relaxed(create_symbolic_link_api);
    if (BOOST_UNLIKELY(!csl_api))
    {
        emit_error(BOOST_ERROR_NOT_SUPPORTED, to, from, ec, "boost::filesystem::create_symlink");
        return;
    }

    if (BOOST_UNLIKELY(!csl_api(from.c_str(), to.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)))
    {
        emit_error(BOOST_ERRNO, to, from, ec, "boost::filesystem::create_symlink");
    }
#endif
}

BOOST_FILESYSTEM_DECL
path current_path(error_code* ec)
{
#if defined(BOOST_FILESYSTEM_USE_WASI)
    // Windows CE has no current directory, so everything's relative to the root of the directory tree.
    // WASI also does not support current path.
    emit_error(BOOST_ERROR_NOT_SUPPORTED, ec, "boost::filesystem::current_path");
    return path();
#elif defined(BOOST_POSIX_API)
    struct local
    {
        static bool getcwd_error(error_code* ec)
        {
            const int err = errno;
            return error((err != ERANGE
#if defined(__MSL__) && (defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__))
                          // bug in some versions of the Metrowerks C lib on the Mac: wrong errno set
                          && err != 0
#endif
                          ) ? err : 0,
                         ec, "boost::filesystem::current_path");
        }
    };

    path cur;
    char small_buf[small_path_size];
    const char* p = ::getcwd(small_buf, sizeof(small_buf));
    if (BOOST_LIKELY(!!p))
    {
        cur = p;
        if (ec)
            ec->clear();
    }
    else if (BOOST_LIKELY(!local::getcwd_error(ec)))
    {
        for (std::size_t path_max = sizeof(small_buf) * 2u;; path_max *= 2u) // loop 'til buffer large enough
        {
            if (BOOST_UNLIKELY(path_max > absolute_path_max))
            {
                emit_error(ENAMETOOLONG, ec, "boost::filesystem::current_path");
                break;
            }

            std::unique_ptr< char[] > buf(new char[path_max]);
            p = ::getcwd(buf.get(), path_max);
            if (BOOST_LIKELY(!!p))
            {
                cur = buf.get();
                if (ec)
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
#else
    DWORD sz;
    if ((sz = ::GetCurrentDirectoryW(0, nullptr)) == 0)
        sz = 1;
    std::unique_ptr< path::value_type[] > buf(new path::value_type[sz]);
    error(::GetCurrentDirectoryW(sz, buf.get()) == 0 ? BOOST_ERRNO : 0, ec, "boost::filesystem::current_path");
    return path(buf.get());
#endif
}

BOOST_FILESYSTEM_DECL
void current_path(path const& p, system::error_code* ec)
{
#if defined(BOOST_FILESYSTEM_USE_WASI)
    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::current_path");
#else
    error(!BOOST_SET_CURRENT_DIRECTORY(p.c_str()) ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::current_path");
#endif
}

BOOST_FILESYSTEM_DECL
bool equivalent_v3(path const& p1, path const& p2, system::error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)

    // p2 is done first, so any error reported is for p1
#if defined(BOOST_FILESYSTEM_USE_STATX)
    struct ::statx s2;
    int e2 = invoke_statx(AT_FDCWD, p2.c_str(), AT_NO_AUTOMOUNT, STATX_INO, &s2);
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
    int e1 = invoke_statx(AT_FDCWD, p1.c_str(), AT_NO_AUTOMOUNT, STATX_INO, &s1);
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
        {
            int err = errno;
            emit_error(err, p1, p2, ec, "boost::filesystem::equivalent");
        }
        return false;
    }

    return equivalent_stat(s1, s2);

#else // Windows

    // Thanks to Jeremy Maitin-Shepard for much help and for permission to
    // base the equivalent() implementation on portions of his
    // file-equivalence-win32.cpp experimental code.

    // Note well: Physical location on external media is part of the
    // equivalence criteria. If there are no open handles, physical location
    // can change due to defragmentation or other relocations. Thus handles
    // must be held open until location information for both paths has
    // been retrieved.

    // p2 is done first, so any error reported is for p1
    unique_handle h2(create_file_handle(
        p2.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));

    unique_handle h1(create_file_handle(
        p1.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));

    if (BOOST_UNLIKELY(!h1 || !h2))
    {
        // if one is invalid and the other isn't, then they aren't equivalent,
        // but if both are invalid then it is an error
        if (!h1 && !h2)
            error(BOOST_ERRNO, p1, p2, ec, "boost::filesystem::equivalent");
        return false;
    }

    // at this point, both handles are known to be valid

    BY_HANDLE_FILE_INFORMATION info1, info2;

    if (error(!::GetFileInformationByHandle(h1.get(), &info1) ? BOOST_ERRNO : 0, p1, p2, ec, "boost::filesystem::equivalent"))
        return false;

    if (error(!::GetFileInformationByHandle(h2.get(), &info2) ? BOOST_ERRNO : 0, p1, p2, ec, "boost::filesystem::equivalent"))
        return false;

    // In theory, volume serial numbers are sufficient to distinguish between
    // devices, but in practice VSN's are sometimes duplicated, so last write
    // time and file size are also checked.
    return info1.dwVolumeSerialNumber == info2.dwVolumeSerialNumber &&
        info1.nFileIndexHigh == info2.nFileIndexHigh &&
        info1.nFileIndexLow == info2.nFileIndexLow &&
        info1.nFileSizeHigh == info2.nFileSizeHigh &&
        info1.nFileSizeLow == info2.nFileSizeLow &&
        info1.ftLastWriteTime.dwLowDateTime == info2.ftLastWriteTime.dwLowDateTime &&
        info1.ftLastWriteTime.dwHighDateTime == info2.ftLastWriteTime.dwHighDateTime;

#endif
}

BOOST_FILESYSTEM_DECL
bool equivalent_v4(path const& p1, path const& p2, system::error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_USE_STATX)
    struct ::statx s1;
    int err = invoke_statx(AT_FDCWD, p1.c_str(), AT_NO_AUTOMOUNT, STATX_INO, &s1);
    if (BOOST_UNLIKELY(err != 0))
    {
    fail_errno:
        err = errno;
    fail_err:
        emit_error(err, p1, p2, ec, "boost::filesystem::equivalent");
        return false;
    }

    if (BOOST_UNLIKELY((s1.stx_mask & STATX_INO) != STATX_INO))
    {
    fail_unsupported:
        err = BOOST_ERROR_NOT_SUPPORTED;
        goto fail_err;
    }

    struct ::statx s2;
    err = invoke_statx(AT_FDCWD, p2.c_str(), AT_NO_AUTOMOUNT, STATX_INO, &s2);
    if (BOOST_UNLIKELY(err != 0))
        goto fail_errno;

    if (BOOST_UNLIKELY((s1.stx_mask & STATX_INO) != STATX_INO))
        goto fail_unsupported;
#else
    struct ::stat s1;
    int err = ::stat(p1.c_str(), &s1);
    if (BOOST_UNLIKELY(err != 0))
    {
    fail_errno:
        err = errno;
        emit_error(err, p1, p2, ec, "boost::filesystem::equivalent");
        return false;
    }

    struct ::stat s2;
    err = ::stat(p2.c_str(), &s2);
    if (BOOST_UNLIKELY(err != 0))
        goto fail_errno;
#endif

    return equivalent_stat(s1, s2);

#else // Windows

    // Thanks to Jeremy Maitin-Shepard for much help and for permission to
    // base the equivalent() implementation on portions of his
    // file-equivalence-win32.cpp experimental code.

    // Note well: Physical location on external media is part of the
    // equivalence criteria. If there are no open handles, physical location
    // can change due to defragmentation or other relocations. Thus handles
    // must be held open until location information for both paths has
    // been retrieved.

    unique_handle h1(create_file_handle(
        p1.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));
    if (BOOST_UNLIKELY(!h1))
    {
    fail_errno:
        err_t err = BOOST_ERRNO;
        emit_error(err, p1, p2, ec, "boost::filesystem::equivalent");
        return false;
    }

    unique_handle h2(create_file_handle(
        p2.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));
    if (BOOST_UNLIKELY(!h2))
        goto fail_errno;

    BY_HANDLE_FILE_INFORMATION info1;
    if (BOOST_UNLIKELY(!::GetFileInformationByHandle(h1.get(), &info1)))
        goto fail_errno;

    BY_HANDLE_FILE_INFORMATION info2;
    if (BOOST_UNLIKELY(!::GetFileInformationByHandle(h2.get(), &info2)))
        goto fail_errno;

    // In theory, volume serial numbers are sufficient to distinguish between
    // devices, but in practice VSN's are sometimes duplicated, so last write
    // time and file size are also checked.
    return info1.dwVolumeSerialNumber == info2.dwVolumeSerialNumber &&
        info1.nFileIndexHigh == info2.nFileIndexHigh &&
        info1.nFileIndexLow == info2.nFileIndexLow &&
        info1.nFileSizeHigh == info2.nFileSizeHigh &&
        info1.nFileSizeLow == info2.nFileSizeLow &&
        info1.ftLastWriteTime.dwLowDateTime == info2.ftLastWriteTime.dwLowDateTime &&
        info1.ftLastWriteTime.dwHighDateTime == info2.ftLastWriteTime.dwHighDateTime;

#endif
}

BOOST_FILESYSTEM_DECL
uintmax_t file_size(path const& p, error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_USE_STATX)
    struct ::statx path_stat;
    int err;
    if (BOOST_UNLIKELY(invoke_statx(AT_FDCWD, p.c_str(), AT_NO_AUTOMOUNT, STATX_TYPE | STATX_SIZE, &path_stat) < 0))
    {
        err = errno;
    fail:
        emit_error(err, p, ec, "boost::filesystem::file_size");
        return static_cast< uintmax_t >(-1);
    }

    if (BOOST_UNLIKELY((path_stat.stx_mask & (STATX_TYPE | STATX_SIZE)) != (STATX_TYPE | STATX_SIZE) || !S_ISREG(path_stat.stx_mode)))
    {
        err = BOOST_ERROR_NOT_SUPPORTED;
        goto fail;
    }
#else
    struct ::stat path_stat;
    int err;
    if (BOOST_UNLIKELY(::stat(p.c_str(), &path_stat) < 0))
    {
        err = errno;
    fail:
        emit_error(err, p, ec, "boost::filesystem::file_size");
        return static_cast< uintmax_t >(-1);
    }

    if (BOOST_UNLIKELY(!S_ISREG(path_stat.st_mode)))
    {
        err = BOOST_ERROR_NOT_SUPPORTED;
        goto fail;
    }
#endif

    return get_size(path_stat);

#else // defined(BOOST_POSIX_API)

    // assume uintmax_t is 64-bits on all Windows compilers

    unique_handle h(create_file_handle(
        p.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));

    DWORD err;
    if (BOOST_UNLIKELY(!h))
    {
    fail_errno:
        err = BOOST_ERRNO;
    fail:
        emit_error(err, p, ec, "boost::filesystem::file_size");
        return static_cast< uintmax_t >(-1);
    }

    BY_HANDLE_FILE_INFORMATION info;
    if (BOOST_UNLIKELY(!::GetFileInformationByHandle(h.get(), &info)))
        goto fail_errno;

    if (BOOST_UNLIKELY((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u))
    {
        err = ERROR_NOT_SUPPORTED;
        goto fail;
    }

    return (static_cast< uintmax_t >(info.nFileSizeHigh) << 32u) | info.nFileSizeLow;

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
uintmax_t hard_link_count(path const& p, system::error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_USE_STATX)
    struct ::statx path_stat;
    if (BOOST_UNLIKELY(invoke_statx(AT_FDCWD, p.c_str(), AT_NO_AUTOMOUNT, STATX_NLINK, &path_stat) < 0))
    {
        emit_error(errno, p, ec, "boost::filesystem::hard_link_count");
        return static_cast< uintmax_t >(-1);
    }

    if (BOOST_UNLIKELY((path_stat.stx_mask & STATX_NLINK) != STATX_NLINK))
    {
        emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::hard_link_count");
        return static_cast< uintmax_t >(-1);
    }

    return static_cast< uintmax_t >(path_stat.stx_nlink);
#else
    struct ::stat path_stat;
    if (BOOST_UNLIKELY(::stat(p.c_str(), &path_stat) < 0))
    {
        emit_error(errno, p, ec, "boost::filesystem::hard_link_count");
        return static_cast< uintmax_t >(-1);
    }

    return static_cast< uintmax_t >(path_stat.st_nlink);
#endif

#else // defined(BOOST_POSIX_API)

    unique_handle h(create_file_handle(
        p.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));

    if (BOOST_UNLIKELY(!h))
    {
    fail_errno:
        emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::hard_link_count");
        return static_cast< uintmax_t >(-1);
    }

    // Link count info is only available through GetFileInformationByHandle
    BY_HANDLE_FILE_INFORMATION info;
    if (BOOST_UNLIKELY(!::GetFileInformationByHandle(h.get(), &info)))
        goto fail_errno;

    return static_cast< uintmax_t >(info.nNumberOfLinks);

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
path initial_path(error_code* ec)
{
    static path init_path;
    if (init_path.empty())
        init_path = current_path(ec);
    else if (ec)
        ec->clear();
    return init_path;
}

//! Tests if the directory is empty. Implemented in directory.cpp.
bool is_empty_directory
(
#if defined(BOOST_POSIX_API)
    boost::scope::unique_fd&& fd,
#else
    unique_handle&& h,
#endif
    path const& p,
    error_code* ec
);

BOOST_FILESYSTEM_DECL
bool is_empty(path const& p, system::error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)

    boost::scope::unique_fd file;
    int err = 0;
    while (true)
    {
        file.reset(::open(p.c_str(), O_RDONLY | O_CLOEXEC));
        if (BOOST_UNLIKELY(!file))
        {
            err = errno;
            if (err == EINTR)
                continue;

        fail:
            emit_error(err, p, ec, "boost::filesystem::is_empty");
            return false;
        }

        break;
    }

#if defined(BOOST_FILESYSTEM_NO_O_CLOEXEC) && defined(FD_CLOEXEC)
    if (BOOST_UNLIKELY(::fcntl(file.get(), F_SETFD, FD_CLOEXEC) < 0))
    {
        err = errno;
        goto fail;
    }
#endif

#if defined(BOOST_FILESYSTEM_USE_STATX)
    struct ::statx path_stat;
    if (BOOST_UNLIKELY(invoke_statx(file.get(), "", AT_EMPTY_PATH | AT_NO_AUTOMOUNT, STATX_TYPE | STATX_SIZE, &path_stat) < 0))
    {
        err = errno;
        goto fail;
    }

    if (BOOST_UNLIKELY((path_stat.stx_mask & (STATX_TYPE | STATX_SIZE)) != (STATX_TYPE | STATX_SIZE)))
    {
        err = BOOST_ERROR_NOT_SUPPORTED;
        goto fail;
    }
#else
    struct ::stat path_stat;
    if (BOOST_UNLIKELY(::fstat(file.get(), &path_stat) < 0))
    {
        err = errno;
        goto fail;
    }
#endif

    const mode_t mode = get_mode(path_stat);
    if (S_ISDIR(mode))
        return is_empty_directory(std::move(file), p, ec);

    if (BOOST_UNLIKELY(!S_ISREG(mode)))
    {
        err = BOOST_ERROR_NOT_SUPPORTED;
        goto fail;
    }

    return get_size(path_stat) == 0u;

#else // defined(BOOST_POSIX_API)

    unique_handle h(create_file_handle(
        p.c_str(),
        FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));

    if (BOOST_UNLIKELY(!h))
    {
    fail_errno:
        const DWORD err = BOOST_ERRNO;
        emit_error(err, p, ec, "boost::filesystem::is_empty");
        return false;
    }

    BY_HANDLE_FILE_INFORMATION info;
    if (BOOST_UNLIKELY(!::GetFileInformationByHandle(h.get(), &info)))
        goto fail_errno;

    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u)
        return is_empty_directory(std::move(h), p, ec);

    return (info.nFileSizeHigh | info.nFileSizeLow) == 0u;

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
std::time_t creation_time(path const& p, system::error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_USE_STATX)
    struct ::statx stx;
    if (BOOST_UNLIKELY(invoke_statx(AT_FDCWD, p.c_str(), AT_NO_AUTOMOUNT, STATX_BTIME, &stx) < 0))
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

    // See the comment in last_write_time regarding access rights used here for GetFileTime.
    unique_handle hw(create_file_handle(
        p.c_str(),
        FILE_READ_ATTRIBUTES | FILE_READ_EA,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));

    DWORD err;
    if (BOOST_UNLIKELY(!hw))
    {
    fail_errno:
        err = BOOST_ERRNO;
    fail:
        emit_error(err, p, ec, "boost::filesystem::creation_time");
        return (std::numeric_limits< std::time_t >::min)();
    }

    FILETIME ct;
    if (BOOST_UNLIKELY(!::GetFileTime(hw.get(), &ct, nullptr, nullptr)))
        goto fail_errno;

    std::time_t t;
    err = to_time_t(ct, t);
    if (BOOST_UNLIKELY(err != 0u))
        goto fail;

    return t;

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
std::time_t last_write_time(path const& p, system::error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_USE_STATX)
    struct ::statx stx;
    if (BOOST_UNLIKELY(invoke_statx(AT_FDCWD, p.c_str(), AT_NO_AUTOMOUNT, STATX_MTIME, &stx) < 0))
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

    // GetFileTime is documented to require GENERIC_READ access right, but this causes problems if the file
    // is opened by another process without FILE_SHARE_READ. In practice, FILE_READ_ATTRIBUTES works, and
    // FILE_READ_EA is also added for good measure, in case if it matters for SMBv1.
    unique_handle hw(create_file_handle(
        p.c_str(),
        FILE_READ_ATTRIBUTES | FILE_READ_EA,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));

    DWORD err;
    if (BOOST_UNLIKELY(!hw))
    {
    fail_errno:
        err = BOOST_ERRNO;
    fail:
        emit_error(err, p, ec, "boost::filesystem::last_write_time");
        return (std::numeric_limits< std::time_t >::min)();
    }

    FILETIME lwt;
    if (BOOST_UNLIKELY(!::GetFileTime(hw.get(), nullptr, nullptr, &lwt)))
        goto fail_errno;

    std::time_t t;
    err = to_time_t(lwt, t);
    if (BOOST_UNLIKELY(err != 0u))
        goto fail;

    return t;

#endif // defined(BOOST_POSIX_API)
}

BOOST_FILESYSTEM_DECL
void last_write_time(path const& p, const std::time_t new_time, system::error_code* ec)
{
    if (ec)
        ec->clear();

#if defined(BOOST_POSIX_API)

#if defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)

    struct timespec times[2] = {};

    // Keep the last access time unchanged
    times[0].tv_nsec = UTIME_OMIT;

    times[1].tv_sec = new_time;

    if (BOOST_UNLIKELY(::utimensat(AT_FDCWD, p.c_str(), times, 0) != 0))
    {
        emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");
        return;
    }

#else // defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)

    struct ::stat st;
    if (BOOST_UNLIKELY(::stat(p.c_str(), &st) < 0))
    {
        emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");
        return;
    }

    ::utimbuf buf;
    buf.actime = st.st_atime; // utime() updates access time too :-(
    buf.modtime = new_time;
    if (BOOST_UNLIKELY(::utime(p.c_str(), &buf) < 0))
        emit_error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");

#endif // defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)

#else // defined(BOOST_POSIX_API)

    unique_handle hw(create_file_handle(
        p.c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS));

    DWORD err;
    if (BOOST_UNLIKELY(!hw))
    {
    fail_errno:
        err = BOOST_ERRNO;
    fail:
        emit_error(err, p, ec, "boost::filesystem::last_write_time");
        return;
    }

    FILETIME lwt;
    err = to_FILETIME(new_time, lwt);
    if (BOOST_UNLIKELY(err != 0u))
        goto fail;

    if (BOOST_UNLIKELY(!::SetFileTime(hw.get(), nullptr, nullptr, &lwt)))
        goto fail_errno;

#endif // defined(BOOST_POSIX_API)
}

#ifdef BOOST_POSIX_API
const perms active_bits(all_all | set_uid_on_exe | set_gid_on_exe | sticky_bit);
inline mode_t mode_cast(perms prms)
{
    return prms & active_bits;
}
#endif

BOOST_FILESYSTEM_DECL
void permissions(path const& p, perms prms, system::error_code* ec)
{
    BOOST_ASSERT_MSG(!((prms & add_perms) && (prms & remove_perms)), "add_perms and remove_perms are mutually exclusive");

    if ((prms & add_perms) && (prms & remove_perms)) // precondition failed
        return;

#if defined(BOOST_FILESYSTEM_USE_WASI)
    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::permissions");
#elif defined(BOOST_POSIX_API)
    error_code local_ec;
    file_status current_status((prms & symlink_perms) ? detail::symlink_status_impl(p, &local_ec) : detail::status_impl(p, &local_ec));
    if (local_ec)
    {
        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::permissions", p, local_ec));

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
#if defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS) && \
    !(defined(__SUNPRO_CC) || defined(__sun) || defined(sun)) && \
    !(defined(linux) || defined(__linux) || defined(__linux__)) && \
    !(defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED < 101000) && \
    !(defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && __IPHONE_OS_VERSION_MIN_REQUIRED < 80000) && \
    !(defined(__rtems__)) && \
    !(defined(__QNX__) && (_NTO_VERSION <= 700))
    if (::fchmodat(AT_FDCWD, p.c_str(), mode_cast(prms), !(prms & symlink_perms) ? 0 : AT_SYMLINK_NOFOLLOW))
#else // fallback if fchmodat() not supported
    if (::chmod(p.c_str(), mode_cast(prms)))
#endif
    {
        const int err = errno;
        if (!ec)
        {
            BOOST_FILESYSTEM_THROW(filesystem_error(
                "boost::filesystem::permissions", p, error_code(err, system::generic_category())));
        }

        ec->assign(err, system::generic_category());
    }

#else // Windows

    // if not going to alter FILE_ATTRIBUTE_READONLY, just return
    if (!(!((prms & (add_perms | remove_perms))) || (prms & (owner_write | group_write | others_write))))
        return;

    DWORD attr = ::GetFileAttributesW(p.c_str());

    if (error(attr == 0 ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::permissions"))
        return;

    if (prms & add_perms)
        attr &= ~FILE_ATTRIBUTE_READONLY;
    else if (prms & remove_perms)
        attr |= FILE_ATTRIBUTE_READONLY;
    else if (prms & (owner_write | group_write | others_write))
        attr &= ~FILE_ATTRIBUTE_READONLY;
    else
        attr |= FILE_ATTRIBUTE_READONLY;

    error(::SetFileAttributesW(p.c_str(), attr) == 0 ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::permissions");
#endif
}

BOOST_FILESYSTEM_DECL
path read_symlink(path const& p, system::error_code* ec)
{
    if (ec)
        ec->clear();

    path symlink_path;

#ifdef BOOST_POSIX_API
    const char* const path_str = p.c_str();
    char small_buf[small_path_size];
    ssize_t result = ::readlink(path_str, small_buf, sizeof(small_buf));
    if (BOOST_UNLIKELY(result < 0))
    {
    fail:
        const int err = errno;
        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::read_symlink", p, error_code(err, system_category())));

        ec->assign(err, system_category());
    }
    else if (BOOST_LIKELY(static_cast< std::size_t >(result) < sizeof(small_buf)))
    {
        symlink_path.assign(small_buf, small_buf + result);
    }
    else
    {
        for (std::size_t path_max = sizeof(small_buf) * 2u;; path_max *= 2u) // loop 'til buffer large enough
        {
            if (BOOST_UNLIKELY(path_max > absolute_path_max))
            {
                if (!ec)
                    BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::read_symlink", p, error_code(ENAMETOOLONG, system_category())));

                ec->assign(ENAMETOOLONG, system_category());
                break;
            }

            std::unique_ptr< char[] > buf(new char[path_max]);
            result = ::readlink(path_str, buf.get(), path_max);
            if (BOOST_UNLIKELY(result < 0))
            {
                goto fail;
            }
            else if (BOOST_LIKELY(static_cast< std::size_t >(result) < path_max))
            {
                symlink_path.assign(buf.get(), buf.get() + result);
                break;
            }
        }
    }

#else

    unique_handle h(create_file_handle(
        p.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));

    DWORD error;
    if (BOOST_UNLIKELY(!h))
    {
    return_last_error:
        error = ::GetLastError();
        emit_error(error, p, ec, "boost::filesystem::read_symlink");
        return symlink_path;
    }

    std::unique_ptr< reparse_data_buffer_with_storage > buf(new reparse_data_buffer_with_storage);
    DWORD sz = 0u;
    if (BOOST_UNLIKELY(!::DeviceIoControl(h.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.get(), sizeof(*buf), &sz, nullptr)))
        goto return_last_error;

    const wchar_t* buffer;
    std::size_t offset, len;
    switch (buf->rdb.ReparseTag)
    {
    case IO_REPARSE_TAG_MOUNT_POINT:
        buffer = buf->rdb.MountPointReparseBuffer.PathBuffer;
        offset = buf->rdb.MountPointReparseBuffer.SubstituteNameOffset;
        len = buf->rdb.MountPointReparseBuffer.SubstituteNameLength;
        break;

    case IO_REPARSE_TAG_SYMLINK:
        buffer = buf->rdb.SymbolicLinkReparseBuffer.PathBuffer;
        offset = buf->rdb.SymbolicLinkReparseBuffer.SubstituteNameOffset;
        len = buf->rdb.SymbolicLinkReparseBuffer.SubstituteNameLength;
        // Note: iff info.rdb.SymbolicLinkReparseBuffer.Flags & SYMLINK_FLAG_RELATIVE
        //       -> resulting path is relative to the source
        break;

    default:
        emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "Unknown ReparseTag in boost::filesystem::read_symlink");
        return symlink_path;
    }

    symlink_path = convert_nt_path_to_win32_path(buffer + offset / sizeof(wchar_t), len / sizeof(wchar_t));
#endif

    return symlink_path;
}

BOOST_FILESYSTEM_DECL
path relative(path const& p, path const& base, error_code* ec)
{
    if (ec)
        ec->clear();

    error_code local_ec;
    path cur_path;
    if (!p.is_absolute() || !base.is_absolute())
    {
        cur_path = detail::current_path(&local_ec);
        if (BOOST_UNLIKELY(!!local_ec))
        {
        fail_local_ec:
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::relative", p, base, local_ec));

            *ec = local_ec;
            return path();
        }
    }

    path wc_base(detail::weakly_canonical_v4(base, cur_path, &local_ec));
    if (BOOST_UNLIKELY(!!local_ec))
        goto fail_local_ec;
    path wc_p(detail::weakly_canonical_v4(p, cur_path, &local_ec));
    if (BOOST_UNLIKELY(!!local_ec))
        goto fail_local_ec;
    return wc_p.lexically_relative(wc_base);
}

BOOST_FILESYSTEM_DECL
bool remove(path const& p, error_code* ec)
{
    if (ec)
        ec->clear();

    return detail::remove_impl(p, ec);
}

BOOST_FILESYSTEM_DECL
uintmax_t remove_all(path const& p, error_code* ec)
{
    if (ec)
        ec->clear();

    return detail::remove_all_impl(p, ec);
}

BOOST_FILESYSTEM_DECL
void rename(path const& old_p, path const& new_p, error_code* ec)
{
    error(!BOOST_MOVE_FILE(old_p.c_str(), new_p.c_str()) ? BOOST_ERRNO : 0, old_p, new_p, ec, "boost::filesystem::rename");
}

BOOST_FILESYSTEM_DECL
void resize_file(path const& p, uintmax_t size, system::error_code* ec)
{
#if defined(BOOST_POSIX_API)
    if (BOOST_UNLIKELY(size > static_cast< uintmax_t >((std::numeric_limits< off_t >::max)())))
    {
        emit_error(system::errc::file_too_large, p, ec, "boost::filesystem::resize_file");
        return;
    }
#endif
    error(!BOOST_RESIZE_FILE(p.c_str(), size) ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::resize_file");
}

BOOST_FILESYSTEM_DECL
space_info space(path const& p, error_code* ec)
{
    space_info info;
    // Initialize members to -1, as required by C++20 [fs.op.space]/1 in case of error
    info.capacity = static_cast< uintmax_t >(-1);
    info.free = static_cast< uintmax_t >(-1);
    info.available = static_cast< uintmax_t >(-1);

    if (ec)
        ec->clear();

#if defined(BOOST_FILESYSTEM_USE_WASI)

    emit_error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "boost::filesystem::space");

#elif defined(BOOST_POSIX_API)

    struct BOOST_STATVFS vfs;
    if (!error(::BOOST_STATVFS(p.c_str(), &vfs) ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::space"))
    {
        info.capacity = static_cast< uintmax_t >(vfs.f_blocks) * BOOST_STATVFS_F_FRSIZE;
        info.free = static_cast< uintmax_t >(vfs.f_bfree) * BOOST_STATVFS_F_FRSIZE;
        info.available = static_cast< uintmax_t >(vfs.f_bavail) * BOOST_STATVFS_F_FRSIZE;
    }

#else

    // GetDiskFreeSpaceExW requires a directory path, which is unlike statvfs, which accepts any file.
    // To work around this, test if the path refers to a directory and use the parent directory if not.
    error_code local_ec;
    file_status status = detail::status_impl(p, &local_ec);
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

        status = detail::symlink_status_impl(p, &local_ec);
        if (status.type() == fs::status_error)
            goto fail_local_ec;
        if (is_symlink(status))
        {
            // We need to resolve the symlink so that we report the space for the symlink target
            dir_path = detail::canonical_v4(p, cur_path, ec);
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
    if (!error(::GetDiskFreeSpaceExW(str.c_str(), &avail, &total, &free) == 0 ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::space"))
    {
        info.capacity = static_cast< uintmax_t >(total.QuadPart);
        info.free = static_cast< uintmax_t >(free.QuadPart);
        info.available = static_cast< uintmax_t >(avail.QuadPart);
    }

#endif

    return info;
}

BOOST_FILESYSTEM_DECL
file_status status(path const& p, error_code* ec)
{
    if (ec)
        ec->clear();

    return detail::status_impl(p, ec);
}

BOOST_FILESYSTEM_DECL
file_status symlink_status(path const& p, error_code* ec)
{
    if (ec)
        ec->clear();

    return detail::symlink_status_impl(p, ec);
}

// contributed by Jeff Flinn
BOOST_FILESYSTEM_DECL
path temp_directory_path(system::error_code* ec)
{
    if (ec)
        ec->clear();

#ifdef BOOST_POSIX_API

    const char* val = nullptr;

    (val = std::getenv("TMPDIR")) ||
        (val = std::getenv("TMP")) ||
        (val = std::getenv("TEMP")) ||
        (val = std::getenv("TEMPDIR"));

#ifdef __ANDROID__
    const char* default_tmp = "/data/local/tmp";
#else
    const char* default_tmp = "/tmp";
#endif
    path p((val != nullptr) ? val : default_tmp);

    if (BOOST_UNLIKELY(p.empty()))
    {
    fail_not_dir:
        error(ENOTDIR, p, ec, "boost::filesystem::temp_directory_path");
        return p;
    }

    file_status status = detail::status_impl(p, ec);
    if (BOOST_UNLIKELY(ec && *ec))
        return path();
    if (BOOST_UNLIKELY(!is_directory(status)))
        goto fail_not_dir;

    return p;

#else // Windows

    static const wchar_t* const env_list[] = { L"TMP", L"TEMP", L"LOCALAPPDATA", L"USERPROFILE" };
    static const wchar_t temp_dir[] = L"Temp";

    path p;
    for (unsigned int i = 0; i < sizeof(env_list) / sizeof(*env_list); ++i)
    {
        std::wstring env = wgetenv(env_list[i]);
        if (!env.empty())
        {
            p = env;
            if (i >= 2)
                path_algorithms::append_v4(p, temp_dir, temp_dir + (sizeof(temp_dir) / sizeof(*temp_dir) - 1u));
            error_code lcl_ec;
            if (exists(p, lcl_ec) && !lcl_ec && is_directory(p, lcl_ec) && !lcl_ec)
                break;
            p.clear();
        }
    }

    if (p.empty())
    {
        // use a separate buffer since in C++03 a string is not required to be contiguous
        const UINT size = ::GetWindowsDirectoryW(nullptr, 0);
        if (BOOST_UNLIKELY(size == 0))
        {
        getwindir_error:
            int errval = ::GetLastError();
            error(errval, ec, "boost::filesystem::temp_directory_path");
            return path();
        }

        std::unique_ptr< wchar_t[] > buf(new wchar_t[size]);
        if (BOOST_UNLIKELY(::GetWindowsDirectoryW(buf.get(), size) == 0))
            goto getwindir_error;

        p = buf.get(); // do not depend on initial buf size, see ticket #10388
        path_algorithms::append_v4(p, temp_dir, temp_dir + (sizeof(temp_dir) / sizeof(*temp_dir) - 1u));
    }

    return p;

#endif
}

BOOST_FILESYSTEM_DECL
path system_complete(path const& p, system::error_code* ec)
{
#ifdef BOOST_POSIX_API

    if (p.empty() || p.is_absolute())
        return p;

    path res(current_path());
    path_algorithms::append_v4(res, p);
    return res;

#else

    if (p.empty())
    {
        if (ec)
            ec->clear();
        return p;
    }

    BOOST_CONSTEXPR_OR_CONST std::size_t buf_size = 128u;
    wchar_t buf[buf_size];
    wchar_t* pfn;
    std::size_t len = get_full_path_name(p, buf_size, buf, &pfn);

    if (error(len == 0 ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::system_complete"))
        return path();

    if (len < buf_size) // len does not include null termination character
        return path(&buf[0]);

    std::unique_ptr< wchar_t[] > big_buf(new wchar_t[len]);

    return error(get_full_path_name(p, len, big_buf.get(), &pfn) == 0 ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::system_complete") ? path() : path(big_buf.get());

#endif
}

BOOST_FILESYSTEM_DECL
path weakly_canonical_v3(path const& p, path const& base, system::error_code* ec)
{
    path source(detail::absolute_v3(p, base, ec));
    if (ec && *ec)
    {
    return_empty_path:
        return path();
    }

    system::error_code local_ec;
    const path::iterator source_end(source.end());

#if defined(BOOST_POSIX_API)

    path::iterator itr(source_end);
    path head(source);
    for (; !head.empty(); path_algorithms::decrement_v4(itr))
    {
        file_status head_status(detail::status_impl(head, &local_ec));
        if (BOOST_UNLIKELY(head_status.type() == fs::status_error))
        {
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::weakly_canonical", head, local_ec));

            *ec = local_ec;
            goto return_empty_path;
        }

        if (head_status.type() != fs::file_not_found)
            break;

        head.remove_filename_and_trailing_separators();
    }

    path const& dot_p = dot_path();
    path const& dot_dot_p = dot_dot_path();

#else

    path const& dot_p = dot_path();
    path const& dot_dot_p = dot_dot_path();

    // On Windows, filesystem APIs such as GetFileAttributesW and CreateFileW perform lexical path normalization
    // internally. As a result, a path like "c:\a\.." can be reported as present even if "c:\a" is not. This would
    // break canonical, as symlink_status that it calls internally would report an error that the file at the
    // intermediate path does not exist. To avoid this, scan the initial path in the forward direction.
    // Also, operate on paths with preferred separators. This can be important on Windows since GetFileAttributesW
    // or CreateFileW, which is called in status() may return "file not found" for paths to network shares and
    // mounted cloud storages that have forward slashes as separators.
    // Also, avoid querying status of the root name such as \\?\c: as CreateFileW returns ERROR_INVALID_FUNCTION for
    // such path. Querying the status of a root name such as c: is also not right as this path refers to the current
    // directory on drive C:, which is not what we want to test for existence anyway.
    path::iterator itr(source.begin());
    path head;
    if (source.has_root_name())
    {
        BOOST_ASSERT(itr != source_end);
        head = *itr;
        path_algorithms::increment_v4(itr);
    }

    if (source.has_root_directory())
    {
        BOOST_ASSERT(itr != source_end);
        // Convert generic separator returned by the iterator for the root directory to
        // the preferred separator.
        head += path::preferred_separator;
        path_algorithms::increment_v4(itr);
    }

    if (!head.empty())
    {
        file_status head_status(detail::status_impl(head, &local_ec));
        if (BOOST_UNLIKELY(head_status.type() == fs::status_error))
        {
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::weakly_canonical", head, local_ec));

            *ec = local_ec;
            goto return_empty_path;
        }

        if (head_status.type() == fs::file_not_found)
        {
            // If the root path does not exist then no path element exists
            itr = source.begin();
            head.clear();
            goto skip_head;
        }
    }

    for (; itr != source_end; path_algorithms::increment_v4(itr))
    {
        path const& source_elem = *itr;

        // Avoid querying status of paths containing dot and dot-dot elements, as this will break
        // if the root name starts with "\\?\".
        if (path_algorithms::compare_v4(source_elem, dot_p) == 0)
            continue;

        if (path_algorithms::compare_v4(source_elem, dot_dot_p) == 0)
        {
            if (head.has_relative_path())
                head.remove_filename_and_trailing_separators();

            continue;
        }

        path_algorithms::append_v4(head, source_elem);

        file_status head_status(detail::status_impl(head, &local_ec));
        if (BOOST_UNLIKELY(head_status.type() == fs::status_error))
        {
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::weakly_canonical", head, local_ec));

            *ec = local_ec;
            goto return_empty_path;
        }

        if (head_status.type() == fs::file_not_found)
        {
            head.remove_filename_and_trailing_separators();
            break;
        }
    }

skip_head:;

#endif

    path tail;
    bool tail_has_dots = false;
    for (; itr != source_end; path_algorithms::increment_v4(itr))
    {
        path const& tail_elem = *itr;
        path_algorithms::append_v4(tail, tail_elem);
        // for a later optimization, track if any dot or dot-dot elements are present
        if (!tail_has_dots && (path_algorithms::compare_v4(tail_elem, dot_p) == 0 || path_algorithms::compare_v4(tail_elem, dot_dot_p) == 0))
            tail_has_dots = true;
    }

    head = detail::canonical_v3(head, base, &local_ec);
    if (BOOST_UNLIKELY(!!local_ec))
    {
        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::weakly_canonical", head, local_ec));

        *ec = local_ec;
        goto return_empty_path;
    }

    if (BOOST_LIKELY(!tail.empty()))
    {
        path_algorithms::append_v4(head, tail);

        // optimization: only normalize if tail had dot or dot-dot element
        if (tail_has_dots)
            head = path_algorithms::lexically_normal_v4(head);
    }

    return head;
}

BOOST_FILESYSTEM_DECL
path weakly_canonical_v4(path const& p, path const& base, system::error_code* ec)
{
    path source(detail::absolute_v4(p, base, ec));
    if (ec && *ec)
    {
    return_empty_path:
        return path();
    }

    system::error_code local_ec;
    const path::iterator source_end(source.end());

#if defined(BOOST_POSIX_API)

    path::iterator itr(source_end);
    path head(source);
    for (; !head.empty(); path_algorithms::decrement_v4(itr))
    {
        file_status head_status(detail::status_impl(head, &local_ec));
        if (BOOST_UNLIKELY(head_status.type() == fs::status_error))
        {
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::weakly_canonical", head, local_ec));

            *ec = local_ec;
            goto return_empty_path;
        }

        if (head_status.type() != fs::file_not_found)
            break;

        head.remove_filename_and_trailing_separators();
    }

    path const& dot_p = dot_path();
    path const& dot_dot_p = dot_dot_path();

#else

    path const& dot_p = dot_path();
    path const& dot_dot_p = dot_dot_path();

    // On Windows, filesystem APIs such as GetFileAttributesW and CreateFileW perform lexical path normalization
    // internally. As a result, a path like "c:\a\.." can be reported as present even if "c:\a" is not. This would
    // break canonical, as symlink_status that it calls internally would report an error that the file at the
    // intermediate path does not exist. To avoid this, scan the initial path in the forward direction.
    // Also, operate on paths with preferred separators. This can be important on Windows since GetFileAttributesW
    // or CreateFileW, which is called in status() may return "file not found" for paths to network shares and
    // mounted cloud storages that have forward slashes as separators.
    // Also, avoid querying status of the root name such as \\?\c: as CreateFileW returns ERROR_INVALID_FUNCTION for
    // such path. Querying the status of a root name such as c: is also not right as this path refers to the current
    // directory on drive C:, which is not what we want to test for existence anyway.
    path::iterator itr(source.begin());
    path head;
    if (source.has_root_name())
    {
        BOOST_ASSERT(itr != source_end);
        head = *itr;
        path_algorithms::increment_v4(itr);
    }

    if (source.has_root_directory())
    {
        BOOST_ASSERT(itr != source_end);
        // Convert generic separator returned by the iterator for the root directory to
        // the preferred separator.
        head += path::preferred_separator;
        path_algorithms::increment_v4(itr);
    }

    if (!head.empty())
    {
        file_status head_status(detail::status_impl(head, &local_ec));
        if (BOOST_UNLIKELY(head_status.type() == fs::status_error))
        {
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::weakly_canonical", head, local_ec));

            *ec = local_ec;
            goto return_empty_path;
        }

        if (head_status.type() == fs::file_not_found)
        {
            // If the root path does not exist then no path element exists
            itr = source.begin();
            head.clear();
            goto skip_head;
        }
    }

    for (; itr != source_end; path_algorithms::increment_v4(itr))
    {
        path const& source_elem = *itr;

        // Avoid querying status of paths containing dot and dot-dot elements, as this will break
        // if the root name starts with "\\?\".
        if (path_algorithms::compare_v4(source_elem, dot_p) == 0)
            continue;

        if (path_algorithms::compare_v4(source_elem, dot_dot_p) == 0)
        {
            if (head.has_relative_path())
                head.remove_filename_and_trailing_separators();

            continue;
        }

        path_algorithms::append_v4(head, source_elem);

        file_status head_status(detail::status_impl(head, &local_ec));
        if (BOOST_UNLIKELY(head_status.type() == fs::status_error))
        {
            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::weakly_canonical", head, local_ec));

            *ec = local_ec;
            goto return_empty_path;
        }

        if (head_status.type() == fs::file_not_found)
        {
            head.remove_filename_and_trailing_separators();
            break;
        }
    }

skip_head:;

#endif

    path tail;
    bool tail_has_dots = false;
    for (; itr != source_end; path_algorithms::increment_v4(itr))
    {
        path const& tail_elem = *itr;
        path_algorithms::append_v4(tail, tail_elem);
        // for a later optimization, track if any dot or dot-dot elements are present
        if (!tail_has_dots && (path_algorithms::compare_v4(tail_elem, dot_p) == 0 || path_algorithms::compare_v4(tail_elem, dot_dot_p) == 0))
            tail_has_dots = true;
    }

    head = detail::canonical_v4(head, base, &local_ec);
    if (BOOST_UNLIKELY(!!local_ec))
    {
        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::weakly_canonical", head, local_ec));

        *ec = local_ec;
        goto return_empty_path;
    }

    if (BOOST_LIKELY(!tail.empty()))
    {
        path_algorithms::append_v4(head, tail);

        // optimization: only normalize if tail had dot or dot-dot element
        if (tail_has_dots)
            head = path_algorithms::lexically_normal_v4(head);
    }

    return head;
}

} // namespace detail
} // namespace filesystem
} // namespace boost

#include <boost/filesystem/detail/footer.hpp>
