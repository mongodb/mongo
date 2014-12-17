//  path.cpp  ----------------------------------------------------------------//

//  Copyright 2005 Beman Dawes

//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/filesystem

//----------------------------------------------------------------------------// 

// define BOOST_FILESYSTEM_SOURCE so that <boost/filesystem/config.hpp> knows
// the library is being built (possibly exporting rather than importing code)
#define BOOST_FILESYSTEM_SOURCE 

#ifndef BOOST_SYSTEM_NO_DEPRECATED 
# define BOOST_SYSTEM_NO_DEPRECATED
#endif

#include <boost/filesystem/v2/config.hpp>

#ifndef BOOST_FILESYSTEM2_NARROW_ONLY

#include <boost/filesystem/v2/path.hpp>
#include <boost/scoped_array.hpp>

#include <locale>
#include <boost/cerrno.hpp>
#include <boost/system/error_code.hpp>

#include <cwchar>     // for std::mbstate_t

#if defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__) 
# include <boost/filesystem/detail/utf8_codecvt_facet.hpp>
#endif


namespace
{
  // std::locale construction can throw (if LC_MESSAGES is wrong, for example),
  // so a static at function scope is used to ensure that exceptions can be
  // caught. (A previous version was at namespace scope, so initialization
  // occurred before main(), preventing exceptions from being caught.)
  std::locale & loc()
  {
#if !defined(macintosh) && !defined(__APPLE__) && !defined(__APPLE_CC__) 
    // ISO C calls this "the locale-specific native environment":
    static std::locale lc("");
#else  // Mac OS
    // "All BSD system functions expect their string parameters to be in UTF-8 encoding
    // and nothing else."
    // See http://developer.apple.com/mac/library/documentation/MacOSX/Conceptual/BPInternational/Articles/FileEncodings.html
    std::locale global_loc = std::locale();  // Mac OS doesn't support locale("")
    static std::locale lc(global_loc,
        new boost::filesystem::detail::utf8_codecvt_facet);  
#endif
    return lc;
  }

  const std::codecvt<wchar_t, char, std::mbstate_t> *&
  converter()
  {
   static const std::codecvt<wchar_t, char, std::mbstate_t> *
     cvtr(
       &std::use_facet<std::codecvt<wchar_t, char, std::mbstate_t> >
        ( loc() ) );
   return cvtr;
  }

  bool locked(false);
} // unnamed namespace

namespace boost
{
  namespace filesystem2
  {
    bool wpath_traits::imbue( const std::locale & new_loc, const std::nothrow_t & )
    {
      if ( locked ) return false;
      locked = true;
      loc() = new_loc;
      converter() = &std::use_facet
        <std::codecvt<wchar_t, char, std::mbstate_t> >( loc() );
      return true;
    }

    void wpath_traits::imbue( const std::locale & new_loc )
    {
      if ( locked ) BOOST_FILESYSTEM_THROW(
        wfilesystem_error(
          "boost::filesystem::wpath_traits::imbue() after lockdown",
          make_error_code( system::errc::not_supported ) ) );
      imbue( new_loc, std::nothrow );
    }

    //namespace detail
    //{
    //  BOOST_FILESYSTEM_DECL
    //  const char * what( const char * sys_err_what,
    //    const path & path1, const path & path2, std::string & target)
    //  {
    //    try
    //    {
    //      if ( target.empty() )
    //      {
    //        target = sys_err_what;
    //        if ( !path1.empty() )
    //        {
    //          target += ": \"";
    //          target += path1.file_string();
    //          target += "\"";
    //        }
    //        if ( !path2.empty() )
    //        {
    //          target += ", \"";
    //          target += path2.file_string();
    //          target += "\"";
    //        }
    //      }
    //      return target.c_str();
    //    }
    //    catch (...)
    //    {
    //      return sys_err_what;
    //    }
    //  }
    //}
    
# ifdef BOOST_POSIX_API

//  Because this is POSIX only code, we don't have to worry about ABI issues
//  described in http://www.boost.org/more/separate_compilation.html

    wpath_traits::external_string_type
    wpath_traits::to_external( const wpath & ph, 
      const internal_string_type & src )
    {
      locked = true;
      std::size_t work_size( converter()->max_length() * (src.size()+1) );
      boost::scoped_array<char> work( new char[ work_size ] );
      std::mbstate_t state = std::mbstate_t();  // perhaps unneeded, but cuts bug reports
      const internal_string_type::value_type * from_next;
      external_string_type::value_type * to_next;
      if ( converter()->out( 
        state, src.c_str(), src.c_str()+src.size(), from_next, work.get(),
        work.get()+work_size, to_next ) != std::codecvt_base::ok )
        BOOST_FILESYSTEM_THROW( boost::filesystem::wfilesystem_error(
          "boost::filesystem::wpath::to_external conversion error",
          ph, system::error_code( system::errc::invalid_argument, system::system_category() ) ) );
      *to_next = '\0';
      return external_string_type( work.get() );
    }

    wpath_traits::internal_string_type
    wpath_traits::to_internal( const external_string_type & src )
    {
      locked = true;
      std::size_t work_size( src.size()+1 );
      boost::scoped_array<wchar_t> work( new wchar_t[ work_size ] );
      std::mbstate_t state  = std::mbstate_t();  // perhaps unneeded, but cuts bug reports
      const external_string_type::value_type * from_next;
      internal_string_type::value_type * to_next;
      if ( converter()->in( 
        state, src.c_str(), src.c_str()+src.size(), from_next, work.get(),
        work.get()+work_size, to_next ) != std::codecvt_base::ok )
        BOOST_FILESYSTEM_THROW( boost::filesystem::wfilesystem_error(
          "boost::filesystem::wpath::to_internal conversion error",
          system::error_code( system::errc::invalid_argument, system::system_category() ) ) );
      *to_next = L'\0';
      return internal_string_type( work.get() );
    }
# endif // BOOST_POSIX_API

  } // namespace filesystem2
} // namespace boost

#endif // ifndef BOOST_FILESYSTEM2_NARROW_ONLY
