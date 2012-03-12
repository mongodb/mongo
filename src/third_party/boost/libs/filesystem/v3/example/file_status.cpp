//  status.cpp  ------------------------------------------------------------------------//

//  Copyright Beman Dawes 2011

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#include <iostream>
#include <boost/config.hpp>
#include <boost/version.hpp>
#include <boost/filesystem.hpp>
#include <boost/detail/lightweight_main.hpp>

using std::cout; using std::endl;
using namespace boost::filesystem;

namespace
{
  path p;
  
  void print_boost_macros()
  {
    std::cout << "Boost "
              << BOOST_VERSION / 100000 << '.'
              << BOOST_VERSION / 100 % 1000 << '.'
              << BOOST_VERSION % 100 << ", "
#           ifndef _WIN64
              << BOOST_COMPILER << ", "
#           else
              << BOOST_COMPILER << " with _WIN64 defined, "
#           endif
              << BOOST_STDLIB << ", "
              << BOOST_PLATFORM
              << std::endl;
  }

  const char* file_type_tab[] = 
    { "status_error", "file_not_found", "regular_file", "directory_file",
      "symlink_file", "block_file", "character_file", "fifo_file", "socket_file",
      "type_unknown"
    };

  const char* file_type_c_str(enum file_type t)
  {
    return file_type_tab[t];
  }

  void show_status(file_status s, boost::system::error_code ec)
  {
    boost::system::error_condition econd;

    if (ec)
    {
      econd = ec.default_error_condition();
      cout << "sets ec to indicate an error:\n"
           << "   ec.value() is " << ec.value() << '\n'
           << "   ec.message() is \"" << ec.message() << "\"\n"
           << "   ec.default_error_condition().value() is " << econd.value() << '\n'
           << "   ec.default_error_condition().message() is \"" << econd.message() << "\"\n";
    }
    else
      cout << "clears ec.\n";

    cout << "s.type() is " << s.type()
         << ", which is defined as \"" << file_type_c_str(s.type()) << "\"\n";

    cout << "exists(s) is " << (exists(s) ? "true" : "false") << "\n";
    cout << "status_known(s) is " << (status_known(s) ? "true" : "false") << "\n";
    cout << "is_regular_file(s) is " << (is_regular_file(s) ? "true" : "false") << "\n";
    cout << "is_directory(s) is " << (is_directory(s) ? "true" : "false") << "\n";
    cout << "is_other(s) is " << (is_other(s) ? "true" : "false") << "\n";
    cout << "is_symlink(s) is " << (is_symlink(s) ? "true" : "false") << "\n";
  }

  void try_exists()
  {
    cout << "\nexists(" << p << ") ";
    try
    {
      bool result = exists(p);
      cout << "is " << (result ? "true" : "false") << "\n";
    }
    catch (const filesystem_error& ex)
    {
      cout << "throws a filesystem_error exception: " << ex.what() << "\n";
    }
  }

}

int cpp_main(int argc, char* argv[])
{
  print_boost_macros();

  if (argc < 2)
  {
    std::cout << "Usage: file_status <path>\n";
    p = argv[0];
  }
  else
    p = argv[1];

  boost::system::error_code ec;
  file_status s = status(p, ec);
  cout << "\nfile_status s = status(" << p << ", ec) ";
  show_status(s, ec);

  s = symlink_status(p, ec);
  cout << "\nfile_status s = symlink_status(" << p << ", ec) ";
  show_status(s, ec);

  try_exists();

  return 0;
}
