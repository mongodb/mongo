//  Boost.Filesystem mbpath.hpp  ---------------------------------------------//

//  Copyright Beman Dawes 2005

//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  Encodes wide character paths as MBCS
//  See http://../doc/path.htm#mbpath for more information

#include <boost/filesystem/path.hpp>
#include <cwchar>     // for std::mbstate_t
#include <string>
#include <locale>

namespace user
{
  struct mbpath_traits;
  
  typedef boost::filesystem::basic_path<std::wstring, mbpath_traits> mbpath;

  struct mbpath_traits
  {
    typedef std::wstring internal_string_type;
    typedef std::string external_string_type;

    static external_string_type to_external( const mbpath & ph,
      const internal_string_type & src );

    static internal_string_type to_internal( const external_string_type & src );

    static void imbue( const std::locale & loc );
  };
} // namespace user

namespace boost
{
  namespace filesystem
  {
    template<> struct is_basic_path<user::mbpath>
      { static const bool value = true; };
  }
}
