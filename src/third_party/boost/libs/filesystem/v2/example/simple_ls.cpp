//  simple_ls program  -------------------------------------------------------//

//  Copyright Jeff Garland and Beman Dawes, 2002

//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/filesystem for documentation.

#define BOOST_FILESYSTEM_VERSION 2

//  As an example program, we don't want to use any deprecated features
#define BOOST_FILESYSTEM_NO_DEPRECATED

#include "boost/filesystem/operations.hpp"
#include "boost/filesystem/path.hpp"
#include "boost/progress.hpp"
#include <iostream>

namespace fs = boost::filesystem;

int main( int argc, char* argv[] )
{
  boost::progress_timer t( std::clog );

  fs::path full_path( fs::initial_path<fs::path>() );

  if ( argc > 1 )
    full_path = fs::system_complete( fs::path( argv[1] ) );
  else
    std::cout << "\nusage:   simple_ls [path]" << std::endl;

  unsigned long file_count = 0;
  unsigned long dir_count = 0;
  unsigned long other_count = 0;
  unsigned long err_count = 0;

  if ( !fs::exists( full_path ) )
  {
    std::cout << "\nNot found: " << full_path.file_string() << std::endl;
    return 1;
  }

  if ( fs::is_directory( full_path ) )
  {
    std::cout << "\nIn directory: "
              << full_path.directory_string() << "\n\n";
    fs::directory_iterator end_iter;
    for ( fs::directory_iterator dir_itr( full_path );
          dir_itr != end_iter;
          ++dir_itr )
    {
      try
      {
        if ( fs::is_directory( dir_itr->status() ) )
        {
          ++dir_count;
          std::cout << dir_itr->path().filename() << " [directory]\n";
        }
        else if ( fs::is_regular_file( dir_itr->status() ) )
        {
          ++file_count;
          std::cout << dir_itr->path().filename() << "\n";
        }
        else
        {
          ++other_count;
          std::cout << dir_itr->path().filename() << " [other]\n";
        }

      }
      catch ( const std::exception & ex )
      {
        ++err_count;
        std::cout << dir_itr->path().filename() << " " << ex.what() << std::endl;
      }
    }
    std::cout << "\n" << file_count << " files\n"
              << dir_count << " directories\n"
              << other_count << " others\n"
              << err_count << " errors\n";
  }
  else // must be a file
  {
    std::cout << "\nFound: " << full_path.file_string() << "\n";    
  }
  return 0;
}
