//  error_handling.hpp  --------------------------------------------------------------------//

//  Copyright 2002-2009, 2014 Beman Dawes
//  Copyright 2019 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM3_SRC_ERROR_HANDLING_HPP_
#define BOOST_FILESYSTEM3_SRC_ERROR_HANDLING_HPP_

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
#define BOOST_ERRNO    errno

#define BOOST_ERROR_FILE_NOT_FOUND ENOENT
#define BOOST_ERROR_ALREADY_EXISTS EEXIST
#define BOOST_ERROR_NOT_SUPPORTED ENOSYS

#else

typedef boost::winapi::DWORD_ err_t;

//  Windows uses a non-0 return to indicate success
#define BOOST_ERRNO    boost::winapi::GetLastError()

#define BOOST_ERROR_FILE_NOT_FOUND boost::winapi::ERROR_FILE_NOT_FOUND_
#define BOOST_ERROR_ALREADY_EXISTS boost::winapi::ERROR_ALREADY_EXISTS_
#define BOOST_ERROR_NOT_SUPPORTED boost::winapi::ERROR_NOT_SUPPORTED_

#endif

//  error handling helpers  ----------------------------------------------------------//

// Implemented in exception.cpp
void emit_error(err_t error_num, system::error_code* ec, const char* message);
void emit_error(err_t error_num, const path& p, system::error_code* ec, const char* message);
void emit_error(err_t error_num, const path& p1, const path& p2, system::error_code* ec, const char* message);

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

inline bool error(err_t error_num, const path& p, system::error_code* ec, const char* message)
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

inline bool error(err_t error_num, const path& p1, const path& p2, system::error_code* ec, const char* message)
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

#endif // BOOST_FILESYSTEM3_SRC_ERROR_HANDLING_HPP_
