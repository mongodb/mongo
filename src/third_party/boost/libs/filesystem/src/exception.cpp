//  boost/filesystem/exception.hpp  -----------------------------------------------------//

//  Copyright Beman Dawes 2003
//  Copyright Andrey Semashev 2019

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#include "platform_config.hpp"

#include <string>
#include <boost/filesystem/config.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/exception.hpp>

#include "error_handling.hpp"

#include <boost/config/abi_prefix.hpp> // must be the last #include

namespace boost {
namespace filesystem {

BOOST_FILESYSTEM_DECL filesystem_error::filesystem_error(const std::string& what_arg, system::error_code ec) :
  system::system_error(ec, what_arg)
{
  try
  {
    m_imp_ptr.reset(new impl());
  }
  catch (...)
  {
    m_imp_ptr.reset();
  }
}

BOOST_FILESYSTEM_DECL filesystem_error::filesystem_error(const std::string& what_arg, const path& path1_arg, system::error_code ec) :
  system::system_error(ec, what_arg)
{
  try
  {
    m_imp_ptr.reset(new impl(path1_arg));
  }
  catch (...)
  {
    m_imp_ptr.reset();
  }
}

BOOST_FILESYSTEM_DECL filesystem_error::filesystem_error(const std::string& what_arg, const path& path1_arg, const path& path2_arg, system::error_code ec) :
  system::system_error(ec, what_arg)
{
  try
  {
    m_imp_ptr.reset(new impl(path1_arg, path2_arg));
  }
  catch (...)
  {
    m_imp_ptr.reset();
  }
}

BOOST_FILESYSTEM_DECL filesystem_error::filesystem_error(filesystem_error const& that) :
  system::system_error(static_cast< system::system_error const& >(that)),
  m_imp_ptr(that.m_imp_ptr)
{
}

BOOST_FILESYSTEM_DECL filesystem_error& filesystem_error::operator= (filesystem_error const& that)
{
  static_cast< system::system_error& >(*this) = static_cast< system::system_error const& >(that);
  m_imp_ptr = that.m_imp_ptr;
  return *this;
}

BOOST_FILESYSTEM_DECL filesystem_error::~filesystem_error() BOOST_NOEXCEPT_OR_NOTHROW
{
}

BOOST_FILESYSTEM_DECL const char* filesystem_error::what() const BOOST_NOEXCEPT_OR_NOTHROW
{
  if (m_imp_ptr.get()) try
  {
    if (m_imp_ptr->m_what.empty())
    {
      m_imp_ptr->m_what = system::system_error::what();
      if (!m_imp_ptr->m_path1.empty())
      {
        m_imp_ptr->m_what += ": \"";
        m_imp_ptr->m_what += m_imp_ptr->m_path1.string();
        m_imp_ptr->m_what += "\"";
      }
      if (!m_imp_ptr->m_path2.empty())
      {
        m_imp_ptr->m_what += ", \"";
        m_imp_ptr->m_what += m_imp_ptr->m_path2.string();
        m_imp_ptr->m_what += "\"";
      }
    }

    return m_imp_ptr->m_what.c_str();
  }
  catch (...)
  {
    m_imp_ptr->m_what.clear();
  }

  return system::system_error::what();
}

BOOST_FILESYSTEM_DECL const path& filesystem_error::get_empty_path() BOOST_NOEXCEPT
{
  static const path empty_path;
  return empty_path;
}

//  error handling helpers declared in error_handling.hpp  -----------------------------------------------------//

void emit_error(err_t error_num, system::error_code* ec, const char* message)
{
  if (!ec)
    BOOST_FILESYSTEM_THROW(filesystem_error(message, system::error_code(error_num, system::system_category())));
  else
    ec->assign(error_num, system::system_category());
}

void emit_error(err_t error_num, const path& p, system::error_code* ec, const char* message)
{
  if (!ec)
    BOOST_FILESYSTEM_THROW(filesystem_error(message, p, system::error_code(error_num, system::system_category())));
  else
    ec->assign(error_num, system::system_category());
}

void emit_error(err_t error_num, const path& p1, const path& p2, system::error_code* ec, const char* message)
{
  if (ec == 0)
    BOOST_FILESYSTEM_THROW(filesystem_error(message, p1, p2, system::error_code(error_num, system::system_category())));
  else
    ec->assign(error_num, system::system_category());
}

} // namespace filesystem
} // namespace boost

#include <boost/config/abi_suffix.hpp> // pops abi_prefix.hpp pragmas
