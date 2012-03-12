//  Boost.Filesystem mbcopy.cpp  ---------------------------------------------//

//  Copyright Beman Dawes 2005

//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  Copy the files in a directory, using mbpath to represent the new file names
//  See http://../doc/path.htm#mbpath for more information

#define BOOST_FILESYSTEM_VERSION 2

//  See deprecated_test for tests of deprecated features
#define BOOST_FILESYSTEM_NO_DEPRECATED

#include <boost/filesystem/config.hpp>
# ifdef BOOST_FILESYSTEM2_NARROW_ONLY
#   error This compiler or standard library does not support wide-character strings or paths
# endif

#include "mbpath.hpp"
#include <iostream>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/detail/utf8_codecvt_facet.hpp>

namespace fs = boost::filesystem;

namespace
{
  // we can't use boost::filesystem::copy_file() because the argument types
  // differ, so provide a not-very-smart replacement.

  void copy_file( const fs::wpath & from, const user::mbpath & to )
  {
    fs::ifstream from_file( from, std::ios_base::in | std::ios_base::binary );
    if ( !from_file ) { std::cout << "input open failed\n"; return; }

    fs::ofstream to_file( to, std::ios_base::out | std::ios_base::binary );
    if ( !to_file ) { std::cout << "output open failed\n"; return; }

    char c;
    while ( from_file.get(c) )
    {
      to_file.put(c);
      if ( to_file.fail() ) { std::cout << "write error\n"; return; }
    }

    if ( !from_file.eof() ) { std::cout << "read error\n"; }
  }
}

int main( int argc, char * argv[] )
{
  if ( argc != 2 )
  {
    std::cout << "Copy files in the current directory to a target directory\n"
              << "Usage: mbcopy <target-dir>\n";
    return 1;
  }

  // For encoding, use Boost UTF-8 codecvt
  std::locale global_loc = std::locale();
  std::locale loc( global_loc, new fs::detail::utf8_codecvt_facet );
  user::mbpath_traits::imbue( loc );

  std::string target_string( argv[1] );
  user::mbpath target_dir( user::mbpath_traits::to_internal( target_string ) );

  if ( !fs::is_directory( target_dir ) )
  {
    std::cout << "Error: " << argv[1] << " is not a directory\n";
    return 1;
  }

  for ( fs::wdirectory_iterator it( L"." );
    it != fs::wdirectory_iterator(); ++it )
  {
    if ( fs::is_regular_file(it->status()) )
    {
      copy_file( *it, target_dir / it->path().filename() );
    }
  }

  return 0;
}





