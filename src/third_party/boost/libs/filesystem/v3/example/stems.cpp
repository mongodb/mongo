//  filesystem example stems.cpp  ------------------------------------------------------//

//  Copyright Beman Dawes 2011

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#include <boost/filesystem.hpp>
#include <iostream>

int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cout << "Usage: stems <path>\n";
    return 1;
  }

  boost::filesystem::path p(argv[1]), name(p.filename());

  for(;;)
  {
    std::cout << "filename " << name  << " has stem " << name.stem()
      << " and extension " << name.extension() << "\n";
    if (name.stem().empty() || name.extension().empty())
      return 0;
    name = name.stem();
  }
}
