//  file_size program  -------------------------------------------------------//

//  Copyright Beman Dawes, 2004

//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/filesystem for documentation.

#include <boost/filesystem/operations.hpp>
#include <iostream>

namespace fs = boost::filesystem;

int main( int argc, char* argv[] )
{

  if ( argc != 2 )
  {
    std::cout << "Usage: file_size path\n";
    return 1;
  }

  std::cout << "sizeof(intmax_t) is " << sizeof(boost::intmax_t) << '\n';

  fs::path p( argv[1] );

  if ( !fs::exists( p ) )
  {
    std::cout << "not found: " << argv[1] << std::endl;
    return 1;
  }

  if ( !fs::is_regular( p ) )
  {
    std::cout << "not a regular file: " << argv[1] << std::endl;
    return 1;
  }
 
  std::cout << "size of " << argv[1] << " is " << fs::file_size( p )
    << std::endl;
  return 0;
}
