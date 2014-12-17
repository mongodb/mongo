//  filesystem tut2.cpp  ---------------------------------------------------------------//

//  Copyright Beman Dawes 2009

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#include <iostream>
#include <boost/filesystem.hpp>
using namespace std;
using namespace boost::filesystem;

int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    cout << "Usage: tut2 path\n";
    return 1;
  }

  path p (argv[1]);   // p reads clearer than argv[1] in the following code

  if (exists(p))    // does p actually exist?
  {
    if (is_regular_file(p))        // is p a regular file?
      cout << p << " size is " << file_size(p) << '\n';

    else if (is_directory(p))      // is p a directory?
      cout << p << " is a directory\n";

    else
      cout << p << " exists, but is neither a regular file nor a directory\n";
  }
  else
    cout << p << " does not exist\n";

  return 0;
}
