//  Boost.Filesystem mbpath.cpp  ---------------------------------------------//

//  (c) Copyright Beman Dawes 2005

//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See Boost.Filesystem home page at http://www.boost.org/libs/filesystem

#include <boost/filesystem/config.hpp>
# ifdef BOOST_FILESYSTEM_NARROW_ONLY
#   error This compiler or standard library does not support wide-character strings or paths
# endif

#include "mbpath.hpp"
#include <boost/system/system_error.hpp>
#include <boost/scoped_array.hpp>

namespace fs = boost::filesystem;

namespace
{
  // ISO C calls this "the locale-specific native environment":
  std::locale loc("");

  const std::codecvt<wchar_t, char, std::mbstate_t> *
    cvt( &std::use_facet<std::codecvt<wchar_t, char, std::mbstate_t> >
           ( loc ) );
}

namespace user
{
  mbpath_traits::external_string_type
  mbpath_traits::to_external( const mbpath & ph,
    const internal_string_type & src )
  {
    std::size_t work_size( cvt->max_length() * (src.size()+1) );
    boost::scoped_array<char> work( new char[ work_size ] );
    std::mbstate_t state;
    const internal_string_type::value_type * from_next;
    external_string_type::value_type * to_next;
    if ( cvt->out( 
      state, src.c_str(), src.c_str()+src.size(), from_next, work.get(),
      work.get()+work_size, to_next ) != std::codecvt_base::ok )
      boost::throw_exception<fs::basic_filesystem_error<mbpath> >(
        fs::basic_filesystem_error<mbpath>(
          "user::mbpath::to_external conversion error",
          ph, boost::system::error_code( EINVAL, boost::system::errno_ecat ) ) );
    *to_next = '\0';
    return external_string_type( work.get() );
  }

  mbpath_traits::internal_string_type 
  mbpath_traits::to_internal( const external_string_type & src )
  {
      std::size_t work_size( src.size()+1 );
      boost::scoped_array<wchar_t> work( new wchar_t[ work_size ] );
      std::mbstate_t state;
      const external_string_type::value_type * from_next;
      internal_string_type::value_type * to_next;
      if ( cvt->in( 
        state, src.c_str(), src.c_str()+src.size(), from_next, work.get(),
        work.get()+work_size, to_next ) != std::codecvt_base::ok )
        boost::throw_exception<fs::basic_filesystem_error<mbpath> >(
          fs::basic_filesystem_error<mbpath>(
            "user::mbpath::to_internal conversion error",
            boost::system::error_code( EINVAL, boost::system::errno_ecat ) ) );
      *to_next = L'\0';
      return internal_string_type( work.get() );
  }

  void mbpath_traits::imbue( const std::locale & new_loc )
  {
    loc = new_loc;
    cvt = &std::use_facet
      <std::codecvt<wchar_t, char, std::mbstate_t> >( loc );
  }

} // namespace user
