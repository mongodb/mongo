//  Boost large_file_support_test.cpp  ---------------------------------------//

//  Copyright Beman Dawes 2004.
//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/filesystem

#define BOOST_FILESYSTEM_VERSION 2

//  See deprecated_test for tests of deprecated features
#ifndef BOOST_FILESYSTEM_NO_DEPRECATED 
# define BOOST_FILESYSTEM_NO_DEPRECATED
#endif
#ifndef BOOST_SYSTEM_NO_DEPRECATED 
# define BOOST_SYSTEM_NO_DEPRECATED
#endif

#include <boost/filesystem/operations.hpp>
namespace fs = boost::filesystem;

#include <iostream>

int main()
{
  if ( fs::detail::possible_large_file_size_support() )
  {
    std::cout << "It appears that file sizes greater that 2 gigabytes are possible\n"
                 "for this configuration on this platform since the operating system\n"
                 "does use a large enough integer type to report large file sizes.\n\n"
                 "Whether or not such support is actually present depends on the OS\n";
    return 0;
  }
  std::cout << "The operating system is using an integer type to report file sizes\n"
               "that can not represent file sizes greater that 2 gigabytes (31-bits).\n"
               "Thus the Filesystem Library will not correctly deal with such large\n"
               "files. If you think that this operatiing system should be able to\n"
               "support large files, please report the problem to the Boost developers\n"
               "mailing list.\n";
  return 1;
}
