//  simple_ls program  -------------------------------------------------------//

//  Copyright Jeff Garland and Beman Dawes, 2002

//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/filesystem for documentation.

#define BOOST_FILESYSTEM_VERSION 3

//  As an example program, we don't want to use any deprecated features
#ifndef BOOST_FILESYSTEM_NO_DEPRECATED 
#  define BOOST_FILESYSTEM_NO_DEPRECATED
#endif
#ifndef BOOST_SYSTEM_NO_DEPRECATED 
#  define BOOST_SYSTEM_NO_DEPRECATED
#endif

#include "boost/filesystem/operations.hpp"
#include "boost/filesystem/path.hpp"
#include "boost/progress.hpp"
#include <iostream>

namespace fs = boost::filesystem;

int main(int argc, char* argv[])
{
  fs::path p(fs::current_path());

  if (argc > 1)
    p = fs::system_complete(argv[1]);
  else
    std::cout << "\nusage:   simple_ls [path]" << std::endl;

  unsigned long file_count = 0;
  unsigned long dir_count = 0;
  unsigned long other_count = 0;
  unsigned long err_count = 0;

  if (!fs::exists(p))
  {
    std::cout << "\nNot found: " << p << std::endl;
    return 1;
  }

  if (fs::is_directory(p))
  {
    std::cout << "\nIn directory: " << p << "\n\n";
    fs::directory_iterator end_iter;
    for (fs::directory_iterator dir_itr(p);
          dir_itr != end_iter;
          ++dir_itr)
    {
      try
      {
        if (fs::is_directory(dir_itr->status()))
        {
          ++dir_count;
          std::cout << dir_itr->path().filename() << " [directory]\n";
        }
        else if (fs::is_regular_file(dir_itr->status()))
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
      catch (const std::exception & ex)
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
    std::cout << "\nFound: " << p << "\n";    
  }
  return 0;
}
