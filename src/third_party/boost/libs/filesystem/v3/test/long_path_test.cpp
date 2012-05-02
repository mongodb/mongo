//  long_path_test.cpp  ----------------------------------------------------------------//

//  Copyright Beman Dawes 2011

//  Distributed under the Boost Software License, Version 1.0.
//  http://www.boost.org/LICENSE_1_0.txt

//  See http://www.boost.org/libs/btree for documentation.

//  See http://msdn.microsoft.com/en-us/library/aa365247%28v=vs.85%29.aspx

#define BOOST_FILESYSTEM_VERSION 3

#include <boost/config/warning_disable.hpp>

#include <boost/filesystem.hpp>
#include <iostream>
#include <string>

using namespace boost::filesystem;

#include <boost/detail/lightweight_test.hpp>
#include <boost/detail/lightweight_main.hpp>

namespace
{
}  // unnamed namespace

int cpp_main(int, char*[])
{

  std::string prefix("d:\\temp\\");
  std::cout << "prefix is " << prefix << '\n';

  const std::size_t safe_size
    = 260 - prefix.size() - 100;  // Windows MAX_PATH is 260

  std::string safe_x_string(safe_size, 'x');
  std::string safe_y_string(safe_size, 'y');
  std::string path_escape("\\\\?\\");

  path x_p(prefix + safe_x_string);
  path y_p(path_escape + prefix + safe_x_string + "\\" + safe_y_string);

  std::cout << "x_p.native().size() is " << x_p.native().size() << '\n';
  std::cout << "y_p.native().size() is " << y_p.native().size() << '\n';

  create_directory(x_p);
  BOOST_TEST(exists(x_p));
  create_directory(y_p);
  BOOST_TEST(exists(y_p));

  //std::cout << "directory x.../y... ready for testing, where ... is " << safe_size
  //          << " repeats of x and y, respectively\n";

  BOOST_TEST(exists(x_p));

  //remove_all(x_p);

  return ::boost::report_errors();
}
