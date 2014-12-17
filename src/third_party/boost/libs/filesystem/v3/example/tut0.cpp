//  filesystem tut0.cpp  ---------------------------------------------------------------//

//  Copyright Beman Dawes 2009

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#include <iostream>
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cout << "Usage: tut0 path\n";
    return 1;
  }

  std::cout << argv[1] << '\n';

  return 0;
}
