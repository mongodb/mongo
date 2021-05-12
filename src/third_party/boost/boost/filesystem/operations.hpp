//  boost/filesystem/operations.hpp  ---------------------------------------------------//

//  Copyright Beman Dawes 2002-2009
//  Copyright Jan Langer 2002
//  Copyright Dietmar Kuehl 2001
//  Copyright Vladimir Prus 2002
//  Copyright Andrey Semashev 2020

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM3_OPERATIONS_HPP
#define BOOST_FILESYSTEM3_OPERATIONS_HPP

#include <boost/config.hpp>

# if defined( BOOST_NO_STD_WSTRING )
#   error Configuration not supported: Boost.Filesystem V3 and later requires std::wstring support
# endif

#include <boost/filesystem/config.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/file_status.hpp>

#ifndef BOOST_FILESYSTEM_NO_DEPRECATED
// These includes are left for backward compatibility and should be included directly by users, as needed
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/directory.hpp>
#endif

#include <boost/detail/bitmask.hpp>
#include <boost/core/scoped_enum.hpp>
#include <boost/system/error_code.hpp>
#include <boost/cstdint.hpp>
#include <string>
#include <ctime>

#include <boost/config/abi_prefix.hpp> // must be the last #include

//--------------------------------------------------------------------------------------//

namespace boost {
namespace filesystem {

struct space_info
{
  // all values are byte counts
  boost::uintmax_t capacity;
  boost::uintmax_t free;      // <= capacity
  boost::uintmax_t available; // <= free
};

BOOST_SCOPED_ENUM_UT_DECLARE_BEGIN(copy_options, unsigned int)
{
  none = 0u,                    // Default. For copy_file: error if the target file exists. For copy: do not recurse, follow symlinks, copy file contents.

  // copy_file options:
  skip_existing = 1u,           // Don't overwrite the existing target file, don't report an error
  overwrite_existing = 1u << 1, // Overwrite existing file
  update_existing = 1u << 2,    // Overwrite existing file if its last write time is older than the replacement file

  // copy options:
  recursive = 1u << 8,          // Recurse into sub-directories
  copy_symlinks = 1u << 9,      // Copy symlinks as symlinks instead of copying the referenced file
  skip_symlinks = 1u << 10,     // Don't copy symlinks
  directories_only = 1u << 11,  // Only copy directory structure, do not copy non-directory files
  create_symlinks = 1u << 12,   // Create symlinks instead of copying files
  create_hard_links = 1u << 13, // Create hard links instead of copying files
  _detail_recursing = 1u << 14  // Internal use only, do not use
}
BOOST_SCOPED_ENUM_DECLARE_END(copy_options)

BOOST_BITMASK(BOOST_SCOPED_ENUM_NATIVE(copy_options))

#if !defined(BOOST_FILESYSTEM_NO_DEPRECATED)
BOOST_SCOPED_ENUM_DECLARE_BEGIN(copy_option)
{
  none = static_cast< unsigned int >(copy_options::none),
  fail_if_exists = none,
  overwrite_if_exists = static_cast< unsigned int >(copy_options::overwrite_existing)
}
BOOST_SCOPED_ENUM_DECLARE_END(copy_option)
#endif

//--------------------------------------------------------------------------------------//
//                             implementation details                                   //
//--------------------------------------------------------------------------------------//

namespace detail {

BOOST_FILESYSTEM_DECL
path absolute(const path& p, const path& base, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
file_status status(const path&p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
file_status symlink_status(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
bool is_empty(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
path initial_path(system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
path canonical(const path& p, const path& base, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
void copy(const path& from, const path& to, unsigned int options, system::error_code* ec=0);
#if !defined(BOOST_FILESYSTEM_NO_DEPRECATED)
BOOST_FILESYSTEM_DECL
void copy_directory(const path& from, const path& to, system::error_code* ec=0);
#endif
BOOST_FILESYSTEM_DECL
bool copy_file(const path& from, const path& to,  // See ticket #2925
               unsigned int options, system::error_code* ec=0); // see copy_options for options
BOOST_FILESYSTEM_DECL
void copy_symlink(const path& existing_symlink, const path& new_symlink, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
bool create_directories(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
bool create_directory(const path& p, const path* existing, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
void create_directory_symlink(const path& to, const path& from,
                              system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
void create_hard_link(const path& to, const path& from, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
void create_symlink(const path& to, const path& from, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
path current_path(system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
void current_path(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
bool equivalent(const path& p1, const path& p2, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
boost::uintmax_t file_size(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
boost::uintmax_t hard_link_count(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
std::time_t creation_time(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
std::time_t last_write_time(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
void last_write_time(const path& p, const std::time_t new_time,
                     system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
void permissions(const path& p, perms prms, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
path read_symlink(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
path relative(const path& p, const path& base, system::error_code* ec = 0);
BOOST_FILESYSTEM_DECL
bool remove(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
boost::uintmax_t remove_all(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
void rename(const path& old_p, const path& new_p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
void resize_file(const path& p, uintmax_t size, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
space_info space(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
path system_complete(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
path temp_directory_path(system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
path unique_path(const path& p, system::error_code* ec=0);
BOOST_FILESYSTEM_DECL
path weakly_canonical(const path& p, system::error_code* ec = 0);

} // namespace detail

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                             status query functions                                   //
//                                                                                      //
//--------------------------------------------------------------------------------------//

inline
file_status status(const path& p)    {return detail::status(p);}
inline
file_status status(const path& p, system::error_code& ec)
                                     {return detail::status(p, &ec);}
inline
file_status symlink_status(const path& p) {return detail::symlink_status(p);}
inline
file_status symlink_status(const path& p, system::error_code& ec)
                                     {return detail::symlink_status(p, &ec);}
inline
bool exists(const path& p)           {return exists(detail::status(p));}
inline
bool exists(const path& p, system::error_code& ec)
                                     {return exists(detail::status(p, &ec));}
inline
bool is_directory(const path& p)     {return is_directory(detail::status(p));}
inline
bool is_directory(const path& p, system::error_code& ec)
                                     {return is_directory(detail::status(p, &ec));}
inline
bool is_regular_file(const path& p)  {return is_regular_file(detail::status(p));}
inline
bool is_regular_file(const path& p, system::error_code& ec)
                                     {return is_regular_file(detail::status(p, &ec));}
inline
bool is_other(const path& p)         {return is_other(detail::status(p));}
inline
bool is_other(const path& p, system::error_code& ec)
                                     {return is_other(detail::status(p, &ec));}
inline
bool is_symlink(const path& p)       {return is_symlink(detail::symlink_status(p));}
inline
bool is_symlink(const path& p, system::error_code& ec)
                                     {return is_symlink(detail::symlink_status(p, &ec));}
#ifndef BOOST_FILESYSTEM_NO_DEPRECATED
inline
bool is_regular(const path& p)       {return is_regular(detail::status(p));}
inline
bool is_regular(const path& p, system::error_code& ec)
                                     {return is_regular(detail::status(p, &ec));}
#endif

inline
bool is_empty(const path& p)         {return detail::is_empty(p);}
inline
bool is_empty(const path& p, system::error_code& ec)
                                     {return detail::is_empty(p, &ec);}

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                             operational functions                                    //
//                                                                                      //
//--------------------------------------------------------------------------------------//

inline
path initial_path()                  {return detail::initial_path();}

inline
path initial_path(system::error_code& ec) {return detail::initial_path(&ec);}

template <class Path>
path initial_path() {return initial_path();}
template <class Path>
path initial_path(system::error_code& ec) {return detail::initial_path(&ec);}

inline
path current_path()                  {return detail::current_path();}

inline
path current_path(system::error_code& ec) {return detail::current_path(&ec);}

inline
void current_path(const path& p)     {detail::current_path(p);}

inline
void current_path(const path& p, system::error_code& ec) BOOST_NOEXCEPT {detail::current_path(p, &ec);}

inline
path absolute(const path& p, const path& base=current_path()) {return detail::absolute(p, base);}
inline
path absolute(const path& p, system::error_code& ec)
{
  path base = current_path(ec);
  if (ec)
    return path();
  return detail::absolute(p, base, &ec);
}
inline
path absolute(const path& p, const path& base, system::error_code& ec) {return detail::absolute(p, base, &ec);}

inline
path canonical(const path& p, const path& base=current_path())
                                     {return detail::canonical(p, base);}
inline
path canonical(const path& p, system::error_code& ec)
{
  path base = current_path(ec);
  if (ec)
    return path();
  return detail::canonical(p, base, &ec);
}
inline
path canonical(const path& p, const path& base, system::error_code& ec)
                                     {return detail::canonical(p, base, &ec);}

#ifndef BOOST_FILESYSTEM_NO_DEPRECATED
inline
path complete(const path& p)
{
  return absolute(p, initial_path());
}

inline
path complete(const path& p, const path& base)
{
  return absolute(p, base);
}
#endif

inline
void copy(const path& from, const path& to)
{
  detail::copy(from, to, static_cast< unsigned int >(copy_options::none));
}
inline
void copy(const path& from, const path& to, system::error_code& ec) BOOST_NOEXCEPT
{
  detail::copy(from, to, static_cast< unsigned int >(copy_options::none), &ec);
}
inline
void copy(const path& from, const path& to, BOOST_SCOPED_ENUM_NATIVE(copy_options) options)
{
  detail::copy(from, to, static_cast< unsigned int >(options));
}
inline
void copy(const path& from, const path& to, BOOST_SCOPED_ENUM_NATIVE(copy_options) options, system::error_code& ec) BOOST_NOEXCEPT
{
  detail::copy(from, to, static_cast< unsigned int >(options), &ec);
}

#if !defined(BOOST_FILESYSTEM_NO_DEPRECATED)
inline
void copy_directory(const path& from, const path& to)
                                     {detail::copy_directory(from, to);}
inline
void copy_directory(const path& from, const path& to, system::error_code& ec) BOOST_NOEXCEPT
                                     {detail::copy_directory(from, to, &ec);}
#endif
inline
bool copy_file(const path& from, const path& to)
{
  return detail::copy_file(from, to, static_cast< unsigned int >(copy_options::none));
}
inline
bool copy_file(const path& from, const path& to, system::error_code& ec) BOOST_NOEXCEPT
{
  return detail::copy_file(from, to, static_cast< unsigned int >(copy_options::none), &ec);
}
inline
bool copy_file(const path& from, const path& to,   // See ticket #2925
               BOOST_SCOPED_ENUM_NATIVE(copy_options) options)
{
  return detail::copy_file(from, to, static_cast< unsigned int >(options));
}
inline
bool copy_file(const path& from, const path& to,   // See ticket #2925
               BOOST_SCOPED_ENUM_NATIVE(copy_options) options, system::error_code& ec) BOOST_NOEXCEPT
{
  return detail::copy_file(from, to, static_cast< unsigned int >(options), &ec);
}
#if !defined(BOOST_FILESYSTEM_NO_DEPRECATED)
inline
bool copy_file(const path& from, const path& to,   // See ticket #2925
               BOOST_SCOPED_ENUM_NATIVE(copy_option) options)
{
  return detail::copy_file(from, to, static_cast< unsigned int >(options));
}
inline
bool copy_file(const path& from, const path& to,   // See ticket #2925
               BOOST_SCOPED_ENUM_NATIVE(copy_option) options, system::error_code& ec) BOOST_NOEXCEPT
{
  return detail::copy_file(from, to, static_cast< unsigned int >(options), &ec);
}
#endif // !defined(BOOST_FILESYSTEM_NO_DEPRECATED)
inline
void copy_symlink(const path& existing_symlink,
                  const path& new_symlink) {detail::copy_symlink(existing_symlink, new_symlink);}

inline
void copy_symlink(const path& existing_symlink, const path& new_symlink,
                  system::error_code& ec) BOOST_NOEXCEPT
                                     {detail::copy_symlink(existing_symlink, new_symlink, &ec);}
inline
bool create_directories(const path& p) {return detail::create_directories(p);}

inline
bool create_directories(const path& p, system::error_code& ec) BOOST_NOEXCEPT
                                     {return detail::create_directories(p, &ec);}
inline
bool create_directory(const path& p) {return detail::create_directory(p, 0);}

inline
bool create_directory(const path& p, system::error_code& ec) BOOST_NOEXCEPT
                                     {return detail::create_directory(p, 0, &ec);}
inline
bool create_directory(const path& p, const path& existing)
                                     {return detail::create_directory(p, &existing);}
inline
bool create_directory(const path& p, const path& existing, system::error_code& ec) BOOST_NOEXCEPT
                                     {return detail::create_directory(p, &existing, &ec);}
inline
void create_directory_symlink(const path& to, const path& from)
                                     {detail::create_directory_symlink(to, from);}
inline
void create_directory_symlink(const path& to, const path& from, system::error_code& ec) BOOST_NOEXCEPT
                                     {detail::create_directory_symlink(to, from, &ec);}
inline
void create_hard_link(const path& to, const path& new_hard_link) {detail::create_hard_link(to, new_hard_link);}

inline
void create_hard_link(const path& to, const path& new_hard_link, system::error_code& ec) BOOST_NOEXCEPT
                                     {detail::create_hard_link(to, new_hard_link, &ec);}
inline
void create_symlink(const path& to, const path& new_symlink) {detail::create_symlink(to, new_symlink);}

inline
void create_symlink(const path& to, const path& new_symlink, system::error_code& ec) BOOST_NOEXCEPT
                                     {detail::create_symlink(to, new_symlink, &ec);}
inline
bool equivalent(const path& p1, const path& p2) {return detail::equivalent(p1, p2);}

inline
bool equivalent(const path& p1, const path& p2, system::error_code& ec) BOOST_NOEXCEPT
                                     {return detail::equivalent(p1, p2, &ec);}
inline
boost::uintmax_t file_size(const path& p) {return detail::file_size(p);}

inline
boost::uintmax_t file_size(const path& p, system::error_code& ec) BOOST_NOEXCEPT
                                     {return detail::file_size(p, &ec);}
inline
boost::uintmax_t hard_link_count(const path& p) {return detail::hard_link_count(p);}

inline
boost::uintmax_t hard_link_count(const path& p, system::error_code& ec) BOOST_NOEXCEPT
                                     {return detail::hard_link_count(p, &ec);}
inline
std::time_t creation_time(const path& p) { return detail::creation_time(p); }

inline
std::time_t creation_time(const path& p, system::error_code& ec) BOOST_NOEXCEPT
                                     { return detail::creation_time(p, &ec); }
inline
std::time_t last_write_time(const path& p) {return detail::last_write_time(p);}

inline
std::time_t last_write_time(const path& p, system::error_code& ec) BOOST_NOEXCEPT
                                     {return detail::last_write_time(p, &ec);}
inline
void last_write_time(const path& p, const std::time_t new_time)
                                     {detail::last_write_time(p, new_time);}
inline
void last_write_time(const path& p, const std::time_t new_time,
                     system::error_code& ec) BOOST_NOEXCEPT
                                     {detail::last_write_time(p, new_time, &ec);}
inline
void permissions(const path& p, perms prms)
                                     {detail::permissions(p, prms);}
inline
void permissions(const path& p, perms prms, system::error_code& ec) BOOST_NOEXCEPT
                                     {detail::permissions(p, prms, &ec);}

inline
path read_symlink(const path& p)     {return detail::read_symlink(p);}

inline
path read_symlink(const path& p, system::error_code& ec)
                                     {return detail::read_symlink(p, &ec);}

inline
bool remove(const path& p)           {return detail::remove(p);}

inline
bool remove(const path& p, system::error_code& ec) BOOST_NOEXCEPT
                                     {return detail::remove(p, &ec);}

inline
boost::uintmax_t remove_all(const path& p) {return detail::remove_all(p);}

inline
boost::uintmax_t remove_all(const path& p, system::error_code& ec) BOOST_NOEXCEPT
                                     {return detail::remove_all(p, &ec);}
inline
void rename(const path& old_p, const path& new_p) {detail::rename(old_p, new_p);}

inline
void rename(const path& old_p, const path& new_p, system::error_code& ec) BOOST_NOEXCEPT
                                     {detail::rename(old_p, new_p, &ec);}
inline  // name suggested by Scott McMurray
void resize_file(const path& p, uintmax_t size) {detail::resize_file(p, size);}

inline
void resize_file(const path& p, uintmax_t size, system::error_code& ec) BOOST_NOEXCEPT
                                     {detail::resize_file(p, size, &ec);}
inline
path relative(const path& p, const path& base=current_path())
                                     {return detail::relative(p, base);}
inline
path relative(const path& p, system::error_code& ec)
{
  path base = current_path(ec);
  if (ec)
    return path();
  return detail::relative(p, base, &ec);
}
inline
path relative(const path& p, const path& base, system::error_code& ec)
                                     {return detail::relative(p, base, &ec);}
inline
space_info space(const path& p)      {return detail::space(p);}

inline
space_info space(const path& p, system::error_code& ec) BOOST_NOEXCEPT
                                     {return detail::space(p, &ec);}

#ifndef BOOST_FILESYSTEM_NO_DEPRECATED
inline bool symbolic_link_exists(const path& p)
                                     { return is_symlink(filesystem::symlink_status(p)); }
#endif

inline
path system_complete(const path& p)  {return detail::system_complete(p);}

inline
path system_complete(const path& p, system::error_code& ec)
                                     {return detail::system_complete(p, &ec);}
inline
path temp_directory_path()           {return detail::temp_directory_path();}

inline
path temp_directory_path(system::error_code& ec)
                                     {return detail::temp_directory_path(&ec);}
inline
path unique_path(const path& p="%%%%-%%%%-%%%%-%%%%")
                                     {return detail::unique_path(p);}
inline
path unique_path(const path& p, system::error_code& ec)
                                     {return detail::unique_path(p, &ec);}
inline
path weakly_canonical(const path& p)   {return detail::weakly_canonical(p);}

inline
path weakly_canonical(const path& p, system::error_code& ec)
                                     {return detail::weakly_canonical(p, &ec);}

//  test helper  -----------------------------------------------------------------------//

//  Not part of the documented interface since false positives are possible;
//  there is no law that says that an OS that has large stat.st_size
//  actually supports large file sizes.

namespace detail {

BOOST_FILESYSTEM_DECL bool possible_large_file_size_support();

} // namespace detail

} // namespace filesystem
} // namespace boost

#include <boost/config/abi_suffix.hpp> // pops abi_prefix.hpp pragmas
#endif // BOOST_FILESYSTEM3_OPERATIONS_HPP
