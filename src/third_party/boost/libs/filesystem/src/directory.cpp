//  directory.cpp  --------------------------------------------------------------------//

//  Copyright 2002-2009, 2014 Beman Dawes
//  Copyright 2001 Dietmar Kuehl
//  Copyright 2019, 2022 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#include "platform_config.hpp"

#include <boost/throw_exception.hpp>
#include <boost/filesystem/config.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/file_status.hpp>

#include <cstddef>
#include <cerrno>
#include <cstring>
#include <cstdlib> // std::malloc, std::free
#include <new>     // std::nothrow, std::bad_alloc
#include <limits>
#include <string>
#include <utility> // std::move
#include <boost/assert.hpp>
#include <boost/system/error_code.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#ifdef BOOST_POSIX_API

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

#if defined(_POSIX_THREAD_SAFE_FUNCTIONS) && (_POSIX_THREAD_SAFE_FUNCTIONS >= 0) && defined(_SC_THREAD_SAFE_FUNCTIONS) && \
    !defined(__CYGWIN__) && \
    !(defined(linux) || defined(__linux) || defined(__linux__)) && \
    !defined(__ANDROID__) && \
    (!defined(__hpux) || defined(_REENTRANT)) && \
    (!defined(_AIX) || defined(__THREAD_SAFE)) && \
    !defined(__wasm)
#define BOOST_FILESYSTEM_USE_READDIR_R
#endif

// At least Mac OS X 10.6 and older doesn't support O_CLOEXEC
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#define BOOST_FILESYSTEM_NO_O_CLOEXEC
#endif

#include "posix_tools.hpp"

#else // BOOST_WINDOWS_API

#include <cwchar>
#include <windows.h>
#include <boost/winapi/basic_types.hpp> // NTSTATUS_

#include "windows_tools.hpp"

#endif // BOOST_WINDOWS_API

#include "atomic_tools.hpp"
#include "error_handling.hpp"
#include "private_config.hpp"

//  BOOST_FILESYSTEM_STATUS_CACHE enables file_status cache in
//  dir_itr_increment. The config tests are placed here because some of the
//  macros being tested come from dirent.h.
//
// TODO: find out what macros indicate dirent::d_type present in more libraries
#if defined(BOOST_WINDOWS_API) || defined(_DIRENT_HAVE_D_TYPE) // defined by GNU C library if d_type present
#define BOOST_FILESYSTEM_STATUS_CACHE
#endif

namespace fs = boost::filesystem;
using boost::system::error_code;
using boost::system::system_category;

namespace boost {
namespace filesystem {

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                 directory_entry                                      //
//                                                                                      //
//--------------------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL
file_status directory_entry::get_status(system::error_code* ec) const
{
    if (!status_known(m_status))
    {
        // optimization: if the symlink status is known, and it isn't a symlink,
        // then status and symlink_status are identical so just copy the
        // symlink status to the regular status.
        if (status_known(m_symlink_status) && !is_symlink(m_symlink_status))
        {
            m_status = m_symlink_status;
            if (ec)
                ec->clear();
        }
        else
        {
            m_status = detail::status(m_path, ec);
        }
    }
    else if (ec)
    {
        ec->clear();
    }

    return m_status;
}

BOOST_FILESYSTEM_DECL
file_status directory_entry::get_symlink_status(system::error_code* ec) const
{
    if (!status_known(m_symlink_status))
        m_symlink_status = detail::symlink_status(m_path, ec);
    else if (ec)
        ec->clear();

    return m_symlink_status;
}

//  dispatch directory_entry supplied here rather than in
//  <boost/filesystem/path_traits.hpp>, thus avoiding header circularity.
//  test cases are in operations_unit_test.cpp

namespace path_traits {

BOOST_FILESYSTEM_DECL
void dispatch(directory_entry const& de,
#ifdef BOOST_WINDOWS_API
              std::wstring& to,
#else
              std::string& to,
#endif
              codecvt_type const&)
{
    to = de.path().native();
}

BOOST_FILESYSTEM_DECL
void dispatch(directory_entry const& de,
#ifdef BOOST_WINDOWS_API
              std::wstring& to
#else
              std::string& to
#endif
)
{
    to = de.path().native();
}

} // namespace path_traits

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                               directory_iterator                                     //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace detail {

BOOST_CONSTEXPR_OR_CONST std::size_t dir_itr_imp_extra_data_alignment = 16u;

BOOST_FILESYSTEM_DECL void* dir_itr_imp::operator new(std::size_t class_size, std::size_t extra_size) BOOST_NOEXCEPT
{
    if (extra_size > 0)
        class_size = (class_size + dir_itr_imp_extra_data_alignment - 1u) & ~(dir_itr_imp_extra_data_alignment - 1u);
    std::size_t total_size = class_size + extra_size;

    // Return NULL on OOM
    void* p = std::malloc(total_size);
    if (BOOST_LIKELY(p != NULL))
        std::memset(p, 0, total_size);
    return p;
}

BOOST_FILESYSTEM_DECL void dir_itr_imp::operator delete(void* p, std::size_t extra_size) BOOST_NOEXCEPT
{
    std::free(p);
}

BOOST_FILESYSTEM_DECL void dir_itr_imp::operator delete(void* p) BOOST_NOEXCEPT
{
    std::free(p);
}

namespace {

inline void* get_dir_itr_imp_extra_data(dir_itr_imp* imp) BOOST_NOEXCEPT
{
    BOOST_CONSTEXPR_OR_CONST std::size_t extra_data_offset = (sizeof(dir_itr_imp) + dir_itr_imp_extra_data_alignment - 1u) & ~(dir_itr_imp_extra_data_alignment - 1u);
    return reinterpret_cast< unsigned char* >(imp) + extra_data_offset;
}

#ifdef BOOST_POSIX_API

inline system::error_code dir_itr_close(dir_itr_imp& imp) BOOST_NOEXCEPT
{
    if (imp.handle != NULL)
    {
        DIR* h = static_cast< DIR* >(imp.handle);
        imp.handle = NULL;
        int err = 0;
        if (BOOST_UNLIKELY(::closedir(h) != 0))
        {
            err = errno;
            return error_code(err, system_category());
        }
    }

    return error_code();
}

#if defined(BOOST_FILESYSTEM_USE_READDIR_R)

// Obtains maximum length of a path, not including the terminating zero
inline std::size_t get_path_max()
{
    // this code is based on Stevens and Rago, Advanced Programming in the
    // UNIX envirnment, 2nd Ed., ISBN 0-201-43307-9, page 49
    std::size_t max = 0;
    errno = 0;
    long res = ::pathconf("/", _PC_PATH_MAX);
    if (res < 0)
    {
#if defined(PATH_MAX)
        max = PATH_MAX;
#else
        max = 4096;
#endif
    }
    else
    {
        max = static_cast< std::size_t >(res); // relative root
#if defined(PATH_MAX)
        if (max < PATH_MAX)
            max = PATH_MAX;
#endif
    }

    if ((max + 1) < sizeof(dirent().d_name))
        max = sizeof(dirent().d_name) - 1;

    return max;
}

// Returns maximum length of a path, not including the terminating zero
inline std::size_t path_max()
{
    static const std::size_t max = get_path_max();
    return max;
}

#endif // BOOST_FILESYSTEM_USE_READDIR_R

// *result set to NULL on end of directory
#if !defined(BOOST_FILESYSTEM_USE_READDIR_R)
inline
#endif
int readdir_impl(dir_itr_imp& imp, struct dirent** result)
{
    errno = 0;

    struct dirent* p = ::readdir(static_cast< DIR* >(imp.handle));
    *result = p;
    if (!p)
        return errno;
    return 0;
}

#if !defined(BOOST_FILESYSTEM_USE_READDIR_R)

inline int invoke_readdir(dir_itr_imp& imp, struct dirent** result)
{
    return readdir_impl(imp, result);
}

#else // !defined(BOOST_FILESYSTEM_USE_READDIR_R)

int readdir_r_impl(dir_itr_imp& imp, struct dirent** result)
{
    return ::readdir_r
    (
        static_cast< DIR* >(imp.handle),
        static_cast< struct dirent* >(get_dir_itr_imp_extra_data(&imp)),
        result
    );
}

int readdir_select_impl(dir_itr_imp& imp, struct dirent** result);

typedef int readdir_impl_t(dir_itr_imp& imp, struct dirent** result);

//! Pointer to the actual implementation of the copy_file_data implementation
readdir_impl_t* readdir_impl_ptr = &readdir_select_impl;

void init_readdir_impl()
{
    readdir_impl_t* impl = &readdir_impl;
    if (::sysconf(_SC_THREAD_SAFE_FUNCTIONS) >= 0)
        impl = &readdir_r_impl;

    filesystem::detail::atomic_store_relaxed(readdir_impl_ptr, impl);
}

struct readdir_initializer
{
    readdir_initializer()
    {
        init_readdir_impl();
    }
};

BOOST_FILESYSTEM_INIT_PRIORITY(BOOST_FILESYSTEM_FUNC_PTR_INIT_PRIORITY) BOOST_ATTRIBUTE_UNUSED BOOST_FILESYSTEM_ATTRIBUTE_RETAIN
const readdir_initializer readdir_init;

int readdir_select_impl(dir_itr_imp& imp, struct dirent** result)
{
    init_readdir_impl();
    return filesystem::detail::atomic_load_relaxed(readdir_impl_ptr)(imp, result);
}

inline int invoke_readdir(dir_itr_imp& imp, struct dirent** result)
{
    return filesystem::detail::atomic_load_relaxed(readdir_impl_ptr)(imp, result);
}

#endif // !defined(BOOST_FILESYSTEM_USE_READDIR_R)

error_code dir_itr_increment(dir_itr_imp& imp, fs::path& filename, fs::file_status& sf, fs::file_status& symlink_sf)
{
    dirent* result = NULL;
    int err = invoke_readdir(imp, &result);
    if (BOOST_UNLIKELY(err != 0))
        return error_code(err, system_category());
    if (result == NULL)
        return dir_itr_close(imp);

    filename = result->d_name;

#ifdef BOOST_FILESYSTEM_STATUS_CACHE
    if (result->d_type == DT_UNKNOWN) // filesystem does not supply d_type value
    {
        sf = symlink_sf = fs::file_status(fs::status_error);
    }
    else // filesystem supplies d_type value
    {
        if (result->d_type == DT_DIR)
            sf = symlink_sf = fs::file_status(fs::directory_file);
        else if (result->d_type == DT_REG)
            sf = symlink_sf = fs::file_status(fs::regular_file);
        else if (result->d_type == DT_LNK)
        {
            sf = fs::file_status(fs::status_error);
            symlink_sf = fs::file_status(fs::symlink_file);
        }
        else
            sf = symlink_sf = fs::file_status(fs::status_error);
    }
#else
    sf = symlink_sf = fs::file_status(fs::status_error);
#endif
    return error_code();
}

error_code dir_itr_create(boost::intrusive_ptr< detail::dir_itr_imp >& imp, fs::path const& dir, unsigned int opts, fs::path& first_filename, fs::file_status&, fs::file_status&)
{
    std::size_t extra_size = 0u;
#if defined(BOOST_FILESYSTEM_USE_READDIR_R)
    {
        readdir_impl_t* rdimpl = filesystem::detail::atomic_load_relaxed(readdir_impl_ptr);
        if (BOOST_UNLIKELY(rdimpl == &readdir_select_impl))
        {
            init_readdir_impl();
            rdimpl = filesystem::detail::atomic_load_relaxed(readdir_impl_ptr);
        }

        if (rdimpl == &readdir_r_impl)
        {
            // According to readdir description, there's no reliable way to predict the length of the d_name string.
            // It may exceed NAME_MAX and pathconf(_PC_NAME_MAX) limits. We are being conservative here and allocate
            // buffer that is enough for PATH_MAX as the directory name. Still, this doesn't guarantee there won't be
            // a buffer overrun. The readdir_r API is fundamentally flawed and we should avoid it as much as possible
            // in favor of readdir.
            extra_size = (sizeof(dirent) - sizeof(dirent().d_name)) + path_max() + 1u; // + 1 for "\0"
        }
    }
#endif // defined(BOOST_FILESYSTEM_USE_READDIR_R)

    boost::intrusive_ptr< detail::dir_itr_imp > pimpl(new (extra_size) detail::dir_itr_imp());
    if (BOOST_UNLIKELY(!pimpl))
        return make_error_code(system::errc::not_enough_memory);

#if defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW)
    int flags = O_DIRECTORY | O_RDONLY | O_NONBLOCK | O_CLOEXEC;
    if ((opts & static_cast< unsigned int >(directory_options::_detail_no_follow)) != 0u)
        flags |= O_NOFOLLOW;

    int fd = ::open(dir.c_str(), flags);
    if (BOOST_UNLIKELY(fd < 0))
    {
        const int err = errno;
        return error_code(err, system_category());
    }

#if defined(BOOST_FILESYSTEM_NO_O_CLOEXEC) && defined(FD_CLOEXEC)
    int res = ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (BOOST_UNLIKELY(res < 0))
    {
        const int err = errno;
        close_fd(fd);
        return error_code(err, system_category());
    }
#endif

    pimpl->handle = ::fdopendir(fd);
    if (BOOST_UNLIKELY(!pimpl->handle))
    {
        const int err = errno;
        close_fd(fd);
        return error_code(err, system_category());
    }
#else // defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW)
    pimpl->handle = ::opendir(dir.c_str());
    if (BOOST_UNLIKELY(!pimpl->handle))
    {
        const int err = errno;
        return error_code(err, system_category());
    }
#endif // defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW)

    // Force initial readdir call by the caller. This will initialize the actual first filename and statuses.
    first_filename.assign(".");

    imp.swap(pimpl);
    return error_code();
}

BOOST_CONSTEXPR_OR_CONST err_t not_found_error_code = ENOENT;

#else // BOOST_WINDOWS_API

inline void set_file_statuses(DWORD attrs, const ULONG* reparse_point_tag, fs::path const& filename, fs::file_status& sf, fs::file_status& symlink_sf)
{
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
    {
        // Reparse points are complex, so don't try to resolve them here; instead just mark
        // them as status_error which causes directory_entry caching to call status()
        // and symlink_status() which do handle reparse points fully
        if (reparse_point_tag)
        {
            // If we have a reparse point tag we can at least populate the symlink status,
            // consistent with symlink_status() behavior
            symlink_sf.type(is_reparse_point_tag_a_symlink(*reparse_point_tag) ? fs::symlink_file : fs::reparse_file);
            symlink_sf.permissions(make_permissions(filename, attrs));
        }
        else
        {
            symlink_sf.type(fs::status_error);
        }

        sf.type(fs::status_error);
    }
    else
    {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0u)
        {
            sf.type(fs::directory_file);
            symlink_sf.type(fs::directory_file);
        }
        else
        {
            sf.type(fs::regular_file);
            symlink_sf.type(fs::regular_file);
        }

        sf.permissions(make_permissions(filename, attrs));
        symlink_sf.permissions(sf.permissions());
    }
}

#if !defined(UNDER_CE)

//! FILE_ID_128 definition from Windows SDK
struct file_id_128
{
    BYTE Identifier[16];
};

//! FILE_DIRECTORY_INFORMATION definition from Windows DDK. Used by NtQueryDirectoryFile, supported since Windows NT 4.0 (probably).
struct file_directory_information
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
};

//! FILE_ID_BOTH_DIR_INFO definition from Windows SDK. Basic support for directory iteration using GetFileInformationByHandleEx, supported since Windows Vista.
struct file_id_both_dir_info
{
    DWORD NextEntryOffset;
    DWORD FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    DWORD FileAttributes;
    DWORD FileNameLength;
    DWORD EaSize;
    CCHAR ShortNameLength;
    WCHAR ShortName[12];
    LARGE_INTEGER FileId;
    WCHAR FileName[1];
};

//! FILE_FULL_DIR_INFO definition from Windows SDK. More lightweight than FILE_ID_BOTH_DIR_INFO, supported since Windows 8.
struct file_full_dir_info
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    WCHAR FileName[1];
};

//! FILE_ID_EXTD_DIR_INFO definition from Windows SDK. Provides reparse point tag, which saves us querying it with a few separate syscalls. Supported since Windows 8.
struct file_id_extd_dir_info
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    ULONG ReparsePointTag;
    file_id_128 FileId;
    WCHAR FileName[1];
};

//! Indicates format of the extra data in the directory iterator
enum extra_data_format
{
    file_directory_information_format,
    file_id_both_dir_info_format,
    file_full_dir_info_format,
    file_id_extd_dir_info_format
};

//! Indicates extra data format that should be used by directory iterator by default
extra_data_format g_extra_data_format = file_directory_information_format;

/*!
 * \brief Extra buffer size for GetFileInformationByHandleEx-based or NtQueryDirectoryFile-based directory iterator.
 * 
 * Must be large enough to accommodate at least one FILE_DIRECTORY_INFORMATION or *_DIR_INFO struct and one filename.
 * NTFS, VFAT, exFAT support filenames up to 255 UTF-16/UCS-2 characters. ReFS supports filenames up to 32768 UTF-16 characters.
 */
BOOST_CONSTEXPR_OR_CONST std::size_t dir_itr_extra_size = sizeof(file_id_extd_dir_info) + 65536u;

inline system::error_code dir_itr_close(dir_itr_imp& imp) BOOST_NOEXCEPT
{
    imp.extra_data_format = 0u;
    imp.current_offset = 0u;

    if (imp.handle != NULL)
    {
        ::CloseHandle(imp.handle);
        imp.handle = NULL;
    }

    return error_code();
}

error_code dir_itr_increment(dir_itr_imp& imp, fs::path& filename, fs::file_status& sf, fs::file_status& symlink_sf)
{
    void* extra_data = get_dir_itr_imp_extra_data(&imp);
    const void* current_data = static_cast< const unsigned char* >(extra_data) + imp.current_offset;
    switch (imp.extra_data_format)
    {
    case file_id_extd_dir_info_format:
        {
            const file_id_extd_dir_info* data = static_cast< const file_id_extd_dir_info* >(current_data);
            if (data->NextEntryOffset == 0u)
            {
                if (!filesystem::detail::atomic_load_relaxed(get_file_information_by_handle_ex_api)(imp.handle, file_id_extd_directory_info_class, extra_data, dir_itr_extra_size))
                {
                    DWORD error = ::GetLastError();

                    dir_itr_close(imp);
                    if (error == ERROR_NO_MORE_FILES)
                        goto done;

                    return error_code(error, system_category());
                }

                imp.current_offset = 0u;
                data = static_cast< const file_id_extd_dir_info* >(extra_data);
            }
            else
            {
                imp.current_offset += data->NextEntryOffset;
                data = reinterpret_cast< const file_id_extd_dir_info* >(static_cast< const unsigned char* >(current_data) + data->NextEntryOffset);
            }

            filename.assign(data->FileName, data->FileName + data->FileNameLength / sizeof(WCHAR));
            set_file_statuses(data->FileAttributes, &data->ReparsePointTag, filename, sf, symlink_sf);
        }
        break;

    case file_full_dir_info_format:
        {
            const file_full_dir_info* data = static_cast< const file_full_dir_info* >(current_data);
            if (data->NextEntryOffset == 0u)
            {
                if (!filesystem::detail::atomic_load_relaxed(get_file_information_by_handle_ex_api)(imp.handle, file_full_directory_info_class, extra_data, dir_itr_extra_size))
                {
                    DWORD error = ::GetLastError();

                    dir_itr_close(imp);
                    if (error == ERROR_NO_MORE_FILES)
                        goto done;

                    return error_code(error, system_category());
                }

                imp.current_offset = 0u;
                data = static_cast< const file_full_dir_info* >(extra_data);
            }
            else
            {
                imp.current_offset += data->NextEntryOffset;
                data = reinterpret_cast< const file_full_dir_info* >(static_cast< const unsigned char* >(current_data) + data->NextEntryOffset);
            }

            filename.assign(data->FileName, data->FileName + data->FileNameLength / sizeof(WCHAR));
            set_file_statuses(data->FileAttributes, NULL, filename, sf, symlink_sf);
        }
        break;

    case file_id_both_dir_info_format:
        {
            const file_id_both_dir_info* data = static_cast< const file_id_both_dir_info* >(current_data);
            if (data->NextEntryOffset == 0u)
            {
                if (!filesystem::detail::atomic_load_relaxed(get_file_information_by_handle_ex_api)(imp.handle, file_id_both_directory_info_class, extra_data, dir_itr_extra_size))
                {
                    DWORD error = ::GetLastError();

                    dir_itr_close(imp);
                    if (error == ERROR_NO_MORE_FILES)
                        goto done;

                    return error_code(error, system_category());
                }

                imp.current_offset = 0u;
                data = static_cast< const file_id_both_dir_info* >(extra_data);
            }
            else
            {
                imp.current_offset += data->NextEntryOffset;
                data = reinterpret_cast< const file_id_both_dir_info* >(static_cast< const unsigned char* >(current_data) + data->NextEntryOffset);
            }

            filename.assign(data->FileName, data->FileName + data->FileNameLength / sizeof(WCHAR));
            set_file_statuses(data->FileAttributes, NULL, filename, sf, symlink_sf);
        }
        break;

    default:
        {
            const file_directory_information* data = static_cast< const file_directory_information* >(current_data);
            if (data->NextEntryOffset == 0u)
            {
                io_status_block iosb;
                boost::winapi::NTSTATUS_ status = filesystem::detail::atomic_load_relaxed(nt_query_directory_file_api)
                (
                    imp.handle,
                    NULL, // Event
                    NULL, // ApcRoutine
                    NULL, // ApcContext
                    &iosb,
                    extra_data,
                    dir_itr_extra_size,
                    file_directory_information_class,
                    FALSE, // ReturnSingleEntry
                    NULL, // FileName
                    FALSE // RestartScan
                );

                if (!NT_SUCCESS(status))
                {
                    dir_itr_close(imp);
                    if (status == STATUS_NO_MORE_FILES)
                        goto done;

                    return error_code(translate_ntstatus(status), system_category());
                }

                imp.current_offset = 0u;
                data = static_cast< const file_directory_information* >(extra_data);
            }
            else
            {
                imp.current_offset += data->NextEntryOffset;
                data = reinterpret_cast< const file_directory_information* >(static_cast< const unsigned char* >(current_data) + data->NextEntryOffset);
            }

            filename.assign(data->FileName, data->FileName + data->FileNameLength / sizeof(WCHAR));
            set_file_statuses(data->FileAttributes, NULL, filename, sf, symlink_sf);
        }
        break;
    }

done:
    return error_code();
}

error_code dir_itr_create(boost::intrusive_ptr< detail::dir_itr_imp >& imp, fs::path const& dir, unsigned int opts, fs::path& first_filename, fs::file_status& sf, fs::file_status& symlink_sf)
{
    boost::intrusive_ptr< detail::dir_itr_imp > pimpl(new (dir_itr_extra_size) detail::dir_itr_imp());
    if (BOOST_UNLIKELY(!pimpl))
        return make_error_code(system::errc::not_enough_memory);

    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if ((opts & static_cast< unsigned int >(directory_options::_detail_no_follow)) != 0u)
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    handle_wrapper h(create_file_handle(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, flags));
    if (BOOST_UNLIKELY(h.handle == INVALID_HANDLE_VALUE))
    {
    return_last_error:
        DWORD error = ::GetLastError();
        return error_code(error, system_category());
    }

    GetFileInformationByHandleEx_t* get_file_information_by_handle_ex = filesystem::detail::atomic_load_relaxed(get_file_information_by_handle_ex_api);
    if (BOOST_LIKELY(get_file_information_by_handle_ex != NULL))
    {
        file_attribute_tag_info info;
        BOOL res = get_file_information_by_handle_ex(h.handle, file_attribute_tag_info_class, &info, sizeof(info));
        if (BOOST_UNLIKELY(!res))
            goto return_last_error;

        if (BOOST_UNLIKELY((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u))
            return make_error_code(system::errc::not_a_directory);

        if ((opts & static_cast< unsigned int >(directory_options::_detail_no_follow)) != 0u)
        {
            if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u && is_reparse_point_tag_a_symlink(info.ReparseTag))
                return make_error_code(system::errc::too_many_symbolic_link_levels);
        }
    }
    else
    {
        BY_HANDLE_FILE_INFORMATION info;
        BOOL res = ::GetFileInformationByHandle(h.handle, &info);
        if (BOOST_UNLIKELY(!res))
            goto return_last_error;

        if (BOOST_UNLIKELY((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u))
            return make_error_code(system::errc::not_a_directory);

        if ((opts & static_cast< unsigned int >(directory_options::_detail_no_follow)) != 0u)
        {
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u && is_reparse_point_a_symlink_ioctl(h.handle))
                return make_error_code(system::errc::too_many_symbolic_link_levels);
        }
    }

    void* extra_data = get_dir_itr_imp_extra_data(pimpl.get());
    switch (filesystem::detail::atomic_load_relaxed(g_extra_data_format))
    {
    case file_id_extd_dir_info_format:
        {
            if (!get_file_information_by_handle_ex(h.handle, file_id_extd_directory_restart_info_class, extra_data, dir_itr_extra_size))
            {
                DWORD error = ::GetLastError();

                if (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER)
                {
                    // Fall back to file_full_dir_info_format.
                    // Note that some mounted filesystems may not support FILE_ID_128 identifiers, which will cause
                    // GetFileInformationByHandleEx(FileIdExtdDirectoryRestartInfo) return ERROR_INVALID_PARAMETER,
                    // even though in general the operation is supported by the kernel. So don't downgrade to
                    // FileFullDirectoryRestartInfo permanently in this case - only for this particular iterator.
                    if (error == ERROR_NOT_SUPPORTED)
                        filesystem::detail::atomic_store_relaxed(g_extra_data_format, file_full_dir_info_format);
                    goto fallback_to_file_full_dir_info_format;
                }

                if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND)
                    goto done;

                return error_code(error, system_category());
            }

            pimpl->extra_data_format = file_id_extd_dir_info_format;

            const file_id_extd_dir_info* data = static_cast< const file_id_extd_dir_info* >(extra_data);
            first_filename.assign(data->FileName, data->FileName + data->FileNameLength / sizeof(WCHAR));

            set_file_statuses(data->FileAttributes, &data->ReparsePointTag, first_filename, sf, symlink_sf);
        }
        break;

    case file_full_dir_info_format:
    fallback_to_file_full_dir_info_format:
        {
            if (!get_file_information_by_handle_ex(h.handle, file_full_directory_restart_info_class, extra_data, dir_itr_extra_size))
            {
                DWORD error = ::GetLastError();

                if (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER)
                {
                    // Fall back to file_id_both_dir_info
                    filesystem::detail::atomic_store_relaxed(g_extra_data_format, file_id_both_dir_info_format);
                    goto fallback_to_file_id_both_dir_info;
                }

                if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND)
                    goto done;

                return error_code(error, system_category());
            }

            pimpl->extra_data_format = file_full_dir_info_format;

            const file_full_dir_info* data = static_cast< const file_full_dir_info* >(extra_data);
            first_filename.assign(data->FileName, data->FileName + data->FileNameLength / sizeof(WCHAR));

            set_file_statuses(data->FileAttributes, NULL, first_filename, sf, symlink_sf);
        }
        break;

    case file_id_both_dir_info_format:
    fallback_to_file_id_both_dir_info:
        {
            if (!get_file_information_by_handle_ex(h.handle, file_id_both_directory_restart_info_class, extra_data, dir_itr_extra_size))
            {
                DWORD error = ::GetLastError();

                if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND)
                    goto done;

                return error_code(error, system_category());
            }

            pimpl->extra_data_format = file_id_both_dir_info_format;

            const file_id_both_dir_info* data = static_cast< const file_id_both_dir_info* >(extra_data);
            first_filename.assign(data->FileName, data->FileName + data->FileNameLength / sizeof(WCHAR));

            set_file_statuses(data->FileAttributes, NULL, first_filename, sf, symlink_sf);
        }
        break;

    default:
        {
            NtQueryDirectoryFile_t* nt_query_directory_file = filesystem::detail::atomic_load_relaxed(boost::filesystem::detail::nt_query_directory_file_api);
            if (BOOST_UNLIKELY(!nt_query_directory_file))
                return error_code(ERROR_NOT_SUPPORTED, system_category());

            io_status_block iosb;
            boost::winapi::NTSTATUS_ status = nt_query_directory_file
            (
                h.handle,
                NULL, // Event
                NULL, // ApcRoutine
                NULL, // ApcContext
                &iosb,
                extra_data,
                dir_itr_extra_size,
                file_directory_information_class,
                FALSE, // ReturnSingleEntry
                NULL, // FileName
                TRUE // RestartScan
            );

            if (!NT_SUCCESS(status))
            {
                // Note: an empty root directory has no "." or ".." entries, so this
                // causes a ERROR_FILE_NOT_FOUND error returned from FindFirstFileW
                // (which is presumably equivalent to STATUS_NO_SUCH_FILE) which we
                // do not consider an error. It is treated as eof instead.
                if (status == STATUS_NO_MORE_FILES || status == STATUS_NO_SUCH_FILE)
                    goto done;

                return error_code(translate_ntstatus(status), system_category());
            }

            pimpl->extra_data_format = file_directory_information_format;

            const file_directory_information* data = static_cast< const file_directory_information* >(extra_data);
            first_filename.assign(data->FileName, data->FileName + data->FileNameLength / sizeof(WCHAR));

            set_file_statuses(data->FileAttributes, NULL, first_filename, sf, symlink_sf);
        }
        break;
    }

    pimpl->handle = h.handle;
    h.handle = INVALID_HANDLE_VALUE;

done:
    imp.swap(pimpl);
    return error_code();
}

#else // !defined(UNDER_CE)

inline system::error_code dir_itr_close(dir_itr_imp& imp) BOOST_NOEXCEPT
{
    if (imp.handle != NULL)
    {
        ::FindClose(imp.handle);
        imp.handle = NULL;
    }

    return error_code();
}

error_code dir_itr_increment(dir_itr_imp& imp, fs::path& filename, fs::file_status& sf, fs::file_status& symlink_sf)
{
    WIN32_FIND_DATAW data;
    if (::FindNextFileW(imp.handle, &data) == 0) // fails
    {
        DWORD error = ::GetLastError();
        dir_itr_close(imp);
        if (error == ERROR_NO_MORE_FILES)
            goto done;
        return error_code(error, system_category());
    }

    filename = data.cFileName;
    set_file_statuses(data.dwFileAttributes, NULL, filename, sf, symlink_sf);

done:
    return error_code();
}

error_code dir_itr_create(boost::intrusive_ptr< detail::dir_itr_imp >& imp, fs::path const& dir, unsigned int opts, fs::path& first_filename, fs::file_status& sf, fs::file_status& symlink_sf)
{
    boost::intrusive_ptr< detail::dir_itr_imp > pimpl(new (static_cast< std::size_t >(0u)) detail::dir_itr_imp());
    if (BOOST_UNLIKELY(!pimpl))
        return make_error_code(system::errc::not_enough_memory);

    // use a form of search Sebastian Martel reports will work with Win98
    fs::path dirpath(dir);
    dirpath.make_preferred();
    dirpath /= L"*";

    WIN32_FIND_DATAW data;
    pimpl->handle = ::FindFirstFileW(dirpath.c_str(), &data);
    if (BOOST_UNLIKELY(pimpl->handle == INVALID_HANDLE_VALUE))
    {
        pimpl->handle = NULL; // signal eof

        // Note: an empty root directory has no "." or ".." entries, so this
        // causes a ERROR_FILE_NOT_FOUND error which we do not consider an
        // error. It is treated as eof instead.
        // Windows Mobile returns ERROR_NO_MORE_FILES; see ticket #3551
        DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES)
            goto done;

        return error_code(error, system_category());
    }

    first_filename = data.cFileName;
    set_file_statuses(data.dwFileAttributes, NULL, first_filename, sf, symlink_sf);

done:
    imp.swap(pimpl);
    return error_code();
}

#endif // !defined(UNDER_CE)

BOOST_CONSTEXPR_OR_CONST err_t not_found_error_code = ERROR_PATH_NOT_FOUND;

#endif // BOOST_WINDOWS_API

} // namespace

#if defined(BOOST_WINDOWS_API) && !defined(UNDER_CE)

//! Initializes directory iterator implementation
void init_directory_iterator_impl() BOOST_NOEXCEPT
{
    if (filesystem::detail::atomic_load_relaxed(get_file_information_by_handle_ex_api) != NULL)
    {
        // Enable the latest format we support. It will get downgraded, if needed, as we attempt
        // to create the directory iterator the first time.
        filesystem::detail::atomic_store_relaxed(g_extra_data_format, file_id_extd_dir_info_format);
    }
}

#endif // defined(BOOST_WINDOWS_API) && !defined(UNDER_CE)

BOOST_FILESYSTEM_DECL
dir_itr_imp::~dir_itr_imp() BOOST_NOEXCEPT
{
    dir_itr_close(*this);
}

BOOST_FILESYSTEM_DECL
void directory_iterator_construct(directory_iterator& it, path const& p, unsigned int opts, system::error_code* ec)
{
    if (BOOST_UNLIKELY(p.empty()))
    {
        emit_error(not_found_error_code, p, ec, "boost::filesystem::directory_iterator::construct");
        return;
    }

    if (ec)
        ec->clear();

    try
    {
        boost::intrusive_ptr< detail::dir_itr_imp > imp;
        path filename;
        file_status file_stat, symlink_file_stat;
        system::error_code result = dir_itr_create(imp, p, opts, filename, file_stat, symlink_file_stat);

        while (true)
        {
            if (result)
            {
                if (result != make_error_condition(system::errc::permission_denied) ||
                    (opts & static_cast< unsigned int >(directory_options::skip_permission_denied)) == 0u)
                {
                    if (!ec)
                        BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::directory_iterator::construct", p, result));
                    *ec = result;
                }

                return;
            }

            if (imp->handle == NULL) // eof, make end
                return;

            // Not eof
            const path::string_type::value_type* filename_str = filename.c_str();
            if (!(filename_str[0] == path::dot // dot or dot-dot
                && (filename_str[1] == static_cast< path::string_type::value_type >('\0') ||
                    (filename_str[1] == path::dot && filename_str[2] == static_cast< path::string_type::value_type >('\0')))))
            {
                imp->dir_entry.assign(p / filename, file_stat, symlink_file_stat);
                it.m_imp.swap(imp);
                return;
            }

            // If dot or dot-dot name produced by the underlying API, skip it until the first actual file
            result = dir_itr_increment(*imp, filename, file_stat, symlink_file_stat);
        }
    }
    catch (std::bad_alloc&)
    {
        if (!ec)
            throw;

        *ec = make_error_code(system::errc::not_enough_memory);
        it.m_imp.reset();
    }
}

BOOST_FILESYSTEM_DECL
void directory_iterator_increment(directory_iterator& it, system::error_code* ec)
{
    BOOST_ASSERT_MSG(!it.is_end(), "attempt to increment end iterator");

    if (ec)
        ec->clear();

    try
    {
        path filename;
        file_status file_stat, symlink_file_stat;
        system::error_code increment_ec;

        while (true)
        {
            increment_ec = dir_itr_increment(*it.m_imp, filename, file_stat, symlink_file_stat);

            if (BOOST_UNLIKELY(!!increment_ec)) // happens if filesystem is corrupt, such as on a damaged optical disc
            {
                boost::intrusive_ptr< detail::dir_itr_imp > imp;
                imp.swap(it.m_imp);
                path error_path(imp->dir_entry.path().parent_path()); // fix ticket #5900
                if (!ec)
                    BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::directory_iterator::operator++", error_path, increment_ec));

                *ec = increment_ec;
                return;
            }

            if (it.m_imp->handle == NULL) // eof, make end
            {
                it.m_imp.reset();
                return;
            }

            const path::string_type::value_type* filename_str = filename.c_str();
            if (!(filename_str[0] == path::dot // !(dot or dot-dot)
                  && (filename_str[1] == static_cast< path::string_type::value_type >('\0') ||
                      (filename_str[1] == path::dot && filename_str[2] == static_cast< path::string_type::value_type >('\0')))))
            {
                it.m_imp->dir_entry.replace_filename(filename, file_stat, symlink_file_stat);
                return;
            }
        }
    }
    catch (std::bad_alloc&)
    {
        if (!ec)
            throw;

        it.m_imp.reset();
        *ec = make_error_code(system::errc::not_enough_memory);
    }
}

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                           recursive_directory_iterator                               //
//                                                                                      //
//--------------------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL
void recursive_directory_iterator_construct(recursive_directory_iterator& it, path const& dir_path, unsigned int opts, system::error_code* ec)
{
    if (ec)
        ec->clear();

    directory_iterator dir_it;
    detail::directory_iterator_construct(dir_it, dir_path, opts, ec);
    if ((ec && *ec) || dir_it == directory_iterator())
        return;

    boost::intrusive_ptr< detail::recur_dir_itr_imp > imp;
    if (!ec)
    {
        imp = new detail::recur_dir_itr_imp(opts);
    }
    else
    {
        imp = new (std::nothrow) detail::recur_dir_itr_imp(opts);
        if (BOOST_UNLIKELY(!imp))
        {
            *ec = make_error_code(system::errc::not_enough_memory);
            return;
        }
    }

    try
    {
#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
        imp->m_stack.push_back(std::move(dir_it));
#else
        imp->m_stack.push_back(dir_it);
#endif

        it.m_imp.swap(imp);
    }
    catch (std::bad_alloc&)
    {
        if (ec)
        {
            *ec = make_error_code(system::errc::not_enough_memory);
            return;
        }

        throw;
    }
}

namespace {

void recursive_directory_iterator_pop_on_error(detail::recur_dir_itr_imp* imp)
{
    imp->m_stack.pop_back();

    while (!imp->m_stack.empty())
    {
        directory_iterator& dir_it = imp->m_stack.back();
        system::error_code increment_ec;
        detail::directory_iterator_increment(dir_it, &increment_ec);
        if (!increment_ec && dir_it != directory_iterator())
            break;

        imp->m_stack.pop_back();
    }
}

} // namespace

BOOST_FILESYSTEM_DECL
void recursive_directory_iterator_pop(recursive_directory_iterator& it, system::error_code* ec)
{
    BOOST_ASSERT_MSG(!it.is_end(), "pop() on end recursive_directory_iterator");
    detail::recur_dir_itr_imp* const imp = it.m_imp.get();

    if (ec)
        ec->clear();

    imp->m_stack.pop_back();

    while (true)
    {
        if (imp->m_stack.empty())
        {
            it.m_imp.reset(); // done, so make end iterator
            break;
        }

        directory_iterator& dir_it = imp->m_stack.back();
        system::error_code increment_ec;
        detail::directory_iterator_increment(dir_it, &increment_ec);
        if (BOOST_UNLIKELY(!!increment_ec))
        {
            if ((imp->m_options & static_cast< unsigned int >(directory_options::pop_on_error)) == 0u)
            {
                // Make an end iterator on errors
                it.m_imp.reset();
            }
            else
            {
                recursive_directory_iterator_pop_on_error(imp);

                if (imp->m_stack.empty())
                    it.m_imp.reset(); // done, so make end iterator
            }

            if (!ec)
                BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::recursive_directory_iterator::pop", increment_ec));

            *ec = increment_ec;
            return;
        }

        if (dir_it != directory_iterator())
            break;

        imp->m_stack.pop_back();
    }
}

namespace {

enum push_directory_result
{
    directory_not_pushed = 0u,
    directory_pushed = 1u,
    keep_depth = 1u << 1
};

// Returns: true if push occurs, otherwise false. Always returns false on error.
inline push_directory_result recursive_directory_iterator_push_directory(detail::recur_dir_itr_imp* imp, system::error_code& ec) BOOST_NOEXCEPT
{
    push_directory_result result = directory_not_pushed;
    try
    {
        //  Discover if the iterator is for a directory that needs to be recursed into,
        //  taking symlinks and options into account.

        if ((imp->m_options & static_cast< unsigned int >(directory_options::_detail_no_push)) != 0u)
        {
            imp->m_options &= ~static_cast< unsigned int >(directory_options::_detail_no_push);
            return result;
        }

        file_status symlink_stat;

        // if we are not recursing into symlinks, we are going to have to know if the
        // stack top is a symlink, so get symlink_status and verify no error occurred
        if ((imp->m_options & static_cast< unsigned int >(directory_options::follow_directory_symlink)) == 0u ||
            (imp->m_options & static_cast< unsigned int >(directory_options::skip_dangling_symlinks)) != 0u)
        {
            symlink_stat = imp->m_stack.back()->symlink_status(ec);
            if (ec)
                return result;
        }

        // Logic for following predicate was contributed by Daniel Aarno to handle cyclic
        // symlinks correctly and efficiently, fixing ticket #5652.
        //   if (((m_options & directory_options::follow_directory_symlink) == directory_options::follow_directory_symlink
        //         || !is_symlink(m_stack.back()->symlink_status()))
        //       && is_directory(m_stack.back()->status())) ...
        // The predicate code has since been rewritten to pass error_code arguments,
        // per ticket #5653.

        if ((imp->m_options & static_cast< unsigned int >(directory_options::follow_directory_symlink)) != 0u || !fs::is_symlink(symlink_stat))
        {
            file_status stat = imp->m_stack.back()->status(ec);
            if (BOOST_UNLIKELY(!!ec))
            {
                if (ec == make_error_condition(system::errc::no_such_file_or_directory) && fs::is_symlink(symlink_stat) &&
                    (imp->m_options & static_cast< unsigned int >(directory_options::follow_directory_symlink | directory_options::skip_dangling_symlinks)) == static_cast< unsigned int >(directory_options::follow_directory_symlink | directory_options::skip_dangling_symlinks))
                {
                    // Skip dangling symlink and continue iteration on the current depth level
                    ec = error_code();
                }

                return result;
            }

            if (!fs::is_directory(stat))
                return result;

            if (BOOST_UNLIKELY((imp->m_stack.size() - 1u) >= static_cast< std::size_t >((std::numeric_limits< int >::max)())))
            {
                // We cannot let depth to overflow
                ec = make_error_code(system::errc::value_too_large);
                // When depth overflow happens, avoid popping the current directory iterator
                // and attempt to continue iteration on the current depth.
                result = keep_depth;
                return result;
            }

            directory_iterator next(imp->m_stack.back()->path(), static_cast< BOOST_SCOPED_ENUM_NATIVE(directory_options) >(imp->m_options), ec);
            if (!ec && next != directory_iterator())
            {
#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
                imp->m_stack.push_back(std::move(next)); // may throw
#else
                imp->m_stack.push_back(next); // may throw
#endif
                return directory_pushed;
            }
        }
    }
    catch (std::bad_alloc&)
    {
        ec = make_error_code(system::errc::not_enough_memory);
    }

    return result;
}

} // namespace

BOOST_FILESYSTEM_DECL
void recursive_directory_iterator_increment(recursive_directory_iterator& it, system::error_code* ec)
{
    BOOST_ASSERT_MSG(!it.is_end(), "increment() on end recursive_directory_iterator");
    detail::recur_dir_itr_imp* const imp = it.m_imp.get();

    if (ec)
        ec->clear();

    system::error_code local_ec;

    //  if various conditions are met, push a directory_iterator into the iterator stack
    push_directory_result push_result = recursive_directory_iterator_push_directory(imp, local_ec);
    if (push_result == directory_pushed)
        return;

    // report errors if any
    if (BOOST_UNLIKELY(!!local_ec))
    {
    on_error:
        if ((imp->m_options & static_cast< unsigned int >(directory_options::pop_on_error)) == 0u)
        {
            // Make an end iterator on errors
            it.m_imp.reset();
        }
        else
        {
            if ((push_result & keep_depth) != 0u)
            {
                system::error_code increment_ec;
                directory_iterator& dir_it = imp->m_stack.back();
                detail::directory_iterator_increment(dir_it, &increment_ec);
                if (!increment_ec && dir_it != directory_iterator())
                    goto on_error_return;
            }

            recursive_directory_iterator_pop_on_error(imp);

            if (imp->m_stack.empty())
                it.m_imp.reset(); // done, so make end iterator
        }

    on_error_return:
        if (!ec)
            BOOST_FILESYSTEM_THROW(filesystem_error("filesystem::recursive_directory_iterator increment error", local_ec));

        *ec = local_ec;
        return;
    }

    //  Do the actual increment operation on the top iterator in the iterator
    //  stack, popping the stack if necessary, until either the stack is empty or a
    //  non-end iterator is reached.
    while (true)
    {
        if (imp->m_stack.empty())
        {
            it.m_imp.reset(); // done, so make end iterator
            break;
        }

        directory_iterator& dir_it = imp->m_stack.back();
        detail::directory_iterator_increment(dir_it, &local_ec);
        if (BOOST_UNLIKELY(!!local_ec))
            goto on_error;

        if (dir_it != directory_iterator())
            break;

        imp->m_stack.pop_back();
    }
}

} // namespace detail

} // namespace filesystem
} // namespace boost
