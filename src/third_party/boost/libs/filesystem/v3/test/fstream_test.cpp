//  fstream_test.cpp  ------------------------------------------------------------------//

//  Copyright Beman Dawes 2002

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#define BOOST_FILESYSTEM_VERSION 3

#include <boost/config.hpp>

# if defined( BOOST_NO_STD_WSTRING )
#   error Configuration not supported: Boost.Filesystem V3 and later requires std::wstring support
# endif

#include <boost/config/warning_disable.hpp>

//  See deprecated_test for tests of deprecated features
#ifndef BOOST_FILESYSTEM_NO_DEPRECATED 
#  define BOOST_FILESYSTEM_NO_DEPRECATED
#endif
#ifndef BOOST_SYSTEM_NO_DEPRECATED 
#  define BOOST_SYSTEM_NO_DEPRECATED
#endif

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <string>
#include <iostream>
#include <cstdio> // for std::remove

#include <boost/filesystem/detail/utf8_codecvt_facet.hpp>

namespace fs = boost::filesystem;

#include <boost/config.hpp>
#ifdef BOOST_NO_STDC_NAMESPACE
  namespace std { using ::remove; }
#endif

#include <boost/detail/lightweight_test.hpp>
#include <boost/detail/lightweight_main.hpp>

#if defined(_MSC_VER)
# pragma warning(push) // Save warning settings.
# pragma warning(disable : 4428) // Disable universal-character-name encountered in source warning.
#endif 

namespace
{
  bool cleanup = true;
  
  void test(const fs::path & p)
  {
    fs::remove(p);
    {
      std::cout << " in test 1\n";
      fs::filebuf fb1;
      fb1.open(p, std::ios_base::out);
      BOOST_TEST(fb1.is_open());
    }
    {
      std::cout << " in test 2\n";
      fs::filebuf fb2;
      fb2.open(p, std::ios_base::in);
      BOOST_TEST(fb2.is_open());
    }
    {
      std::cout << " in test 3\n";
      fs::ifstream tfs(p);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 4\n";
      fs::ifstream tfs(p / p.filename()); // should fail
      BOOST_TEST(!tfs.is_open());
    }
    {
      std::cout << " in test 5\n";
      fs::ifstream tfs(p, std::ios_base::in);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 6\n";
      fs::ifstream tfs;
      tfs.open(p);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 7\n";
      fs::ifstream tfs;
      tfs.open(p, std::ios_base::in);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 8\n";
      fs::ofstream tfs(p);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 9\n";
      fs::ofstream tfs(p, std::ios_base::out);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 10\n";
      fs::ofstream tfs;
      tfs.open(p);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 11\n";
      fs::ofstream tfs;
      tfs.open(p, std::ios_base::out);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 12\n";
      fs::fstream tfs(p);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 13\n";
      fs::fstream tfs(p, std::ios_base::in|std::ios_base::out);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 14\n";
      fs::fstream tfs;
      tfs.open(p);
      BOOST_TEST(tfs.is_open());
    }
    {
      std::cout << " in test 15\n";
      fs::fstream tfs;
      tfs.open(p, std::ios_base::in|std::ios_base::out);
      BOOST_TEST(tfs.is_open());
    }

    if (cleanup)
      fs::remove(p);

  } // test
} // unnamed namespace

int cpp_main(int argc, char*[])
{
  if (argc > 1) cleanup = false;

  std::cout << "BOOST_FILESYSTEM_C_STR defined as \""
            << BOOST_STRINGIZE(BOOST_FILESYSTEM_C_STR) << "\"\n";

  // test narrow characters
  std::cout << "narrow character tests:\n";
  test("v3_fstream_test");


  // So that tests are run with known encoding, use Boost UTF-8 codecvt
  std::locale global_loc = std::locale();
  std::locale loc(global_loc, new fs::detail::utf8_codecvt_facet);
  fs::path::imbue(loc);

  // test with some wide characters
  //  \u2780 is circled 1 against white background == e2 9e 80 in UTF-8
  //  \u2781 is circled 2 against white background == e2 9e 81 in UTF-8
  //  \u263A is a white smiling face
  std::cout << "\nwide character tests:\n";
  test(L"fstream_test_\u2780\u263A");

  return ::boost::report_errors();
}
