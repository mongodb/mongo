//  codecvt_error_category implementation file  ----------------------------------------//

//  Copyright Beman Dawes 2009

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt)

//  Library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#include "platform_config.hpp"

#include <boost/config/warning_disable.hpp>

#include <boost/filesystem/path_traits.hpp>
#include <boost/system/error_code.hpp>
#include <locale>
#include <vector>
#include <cstdlib>
#include <cassert>

//--------------------------------------------------------------------------------------//

namespace
{
  class codecvt_error_cat : public boost::system::error_category
  {
  public:
    codecvt_error_cat(){}
    const char*   name() const BOOST_SYSTEM_NOEXCEPT BOOST_OVERRIDE;
    std::string    message(int ev) const BOOST_OVERRIDE;
  };

  const char* codecvt_error_cat::name() const BOOST_SYSTEM_NOEXCEPT
  {
    return "codecvt";
  }

  std::string codecvt_error_cat::message(int ev) const
  {
    std::string str;
    switch (ev)
    {
    case std::codecvt_base::ok:
      str = "ok";
      break;
    case std::codecvt_base::partial:
      str = "partial";
      break;
    case std::codecvt_base::error:
      str = "error";
      break;
    case std::codecvt_base::noconv:
      str = "noconv";
      break;
    default:
      str = "unknown error";
    }
    return str;
  }

} // unnamed namespace

namespace boost
{
  namespace filesystem
  {

    BOOST_FILESYSTEM_DECL const boost::system::error_category& codecvt_error_category()
    {
      static const codecvt_error_cat  codecvt_error_cat_const;
      return codecvt_error_cat_const;
    }

  } // namespace filesystem
} // namespace boost
