//  posix_tools.hpp  -------------------------------------------------------------------//

//  Copyright 2021-2024 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM_SRC_POSIX_TOOLS_HPP_
#define BOOST_FILESYSTEM_SRC_POSIX_TOOLS_HPP_

#include "platform_config.hpp"
#include <boost/filesystem/config.hpp>
#include <unistd.h>
#include <fcntl.h>

#include <boost/scope/unique_fd.hpp>
#include <boost/system/error_code.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/detail/header.hpp> // must be the last #include

namespace boost {
namespace filesystem {
namespace detail {

//! Platform-specific parameters for directory iterator construction
struct directory_iterator_params
{
#if defined(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW)
    //! File descriptor of the directory to iterate over. If not a negative value, the directory path is only used to generate paths returned by the iterator.
    boost::scope::unique_fd dir_fd;
#endif
};

//! status() implementation
file_status status_impl
(
    path const& p,
    system::error_code* ec
#if defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS) || defined(BOOST_FILESYSTEM_USE_STATX)
    , int basedir_fd = AT_FDCWD
#endif
);

//! symlink_status() implementation
file_status symlink_status_impl
(
    path const& p,
    system::error_code* ec
#if defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS) || defined(BOOST_FILESYSTEM_USE_STATX)
    , int basedir_fd = AT_FDCWD
#endif
);

#if defined(BOOST_POSIX_API)

//! Opens a directory file and returns a file descriptor. Returns a negative value in case of error.
boost::scope::unique_fd open_directory(path const& p, directory_options opts, system::error_code& ec);

#if defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)
//! Opens a directory file and returns a file descriptor. Returns a negative value in case of error.
boost::scope::unique_fd openat_directory(int basedir_fd, path const& p, directory_options opts, system::error_code& ec);
#endif // defined(BOOST_FILESYSTEM_HAS_POSIX_AT_APIS)

#endif // defined(BOOST_POSIX_API)

} // namespace detail
} // namespace filesystem
} // namespace boost

#include <boost/filesystem/detail/footer.hpp>

#endif // BOOST_FILESYSTEM_SRC_POSIX_TOOLS_HPP_
