//  equivalent program -------------------------------------------------------//

//  Copyright (c) 2004 Beman Dawes

//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy
//  at http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/filesystem

//----------------------------------------------------------------------------//

#include <boost/filesystem/operations.hpp>
#include <iostream>
#include <exception>

int main( int argc, char * argv[] )
{
  boost::filesystem::path::default_name_check( boost::filesystem::native );
  if ( argc != 3 )
  {
    std::cout << "Usage: equivalent path1 path2\n";
    return 2;
  }
  
  bool eq;
  try
  {
    eq = boost::filesystem::equivalent( argv[1], argv[2] );
  }
  catch ( const std::exception & ex )
  {
    std::cout << ex.what() << "\n";
    return 3;
  }

  std::cout << (eq ? "Paths are equivalent\n" : "Paths are not equivalent\n");
  return !eq;
}
