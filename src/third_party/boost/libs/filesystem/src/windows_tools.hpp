//  windows_tools.hpp  -----------------------------------------------------------------//

//  Copyright 2002-2009, 2014 Beman Dawes
//  Copyright 2001 Dietmar Kuehl

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM_SRC_WINDOWS_TOOLS_HPP_
#define BOOST_FILESYSTEM_SRC_WINDOWS_TOOLS_HPP_

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/file_status.hpp>

#include <windows.h>

namespace boost {
namespace filesystem {
namespace detail {

inline bool equal_extension(wchar_t const* p, wchar_t const (&x1)[5], wchar_t const (&x2)[5])
{
  return
    (p[0] == x1[0] || p[0] == x2[0]) &&
    (p[1] == x1[1] || p[1] == x2[1]) &&
    (p[2] == x1[2] || p[2] == x2[2]) &&
    (p[3] == x1[3] || p[3] == x2[3]) &&
    p[4] == 0;
}

inline boost::filesystem::perms make_permissions(const boost::filesystem::path& p, DWORD attr)
{
  boost::filesystem::perms prms = boost::filesystem::owner_read | boost::filesystem::group_read | boost::filesystem::others_read;
  if  ((attr & FILE_ATTRIBUTE_READONLY) == 0u)
    prms |= boost::filesystem::owner_write | boost::filesystem::group_write | boost::filesystem::others_write;
  boost::filesystem::path ext = p.extension();
  wchar_t const* q = ext.c_str();
  if (equal_extension(q, L".exe", L".EXE")
    || equal_extension(q, L".com", L".COM")
    || equal_extension(q, L".bat", L".BAT")
    || equal_extension(q, L".cmd", L".CMD"))
    prms |= boost::filesystem::owner_exe | boost::filesystem::group_exe | boost::filesystem::others_exe;
  return prms;
}

} // namespace detail
} // namespace filesystem
} // namespace boost

#endif // BOOST_FILESYSTEM_SRC_WINDOWS_TOOLS_HPP_
