//  path_test program  -----------------------------------------------------------------//

//  Copyright Beman Dawes 2002, 2008
//  Copyright Vladimir Prus 2002

//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/filesystem

//  basic_path's stem(), extension(), and replace_extension() tests are based
//  on basename(), extension(), and change_extension() tests from the original
//  convenience_test.cpp by Vladimir Prus.

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                     Caution                                          //
//                                                                                      //
//  The class path relational operators (==, !=, <, etc.) on Windows treat slash and    //
//  backslash as equal. Thus any tests on Windows where the difference between slash    //
//  and backslash is significant should compare strings rather than paths.              //
//                                                                                      //
//  BOOST_TEST(path == path)                     // '\\' and '/' are equal              //
//  BOOST_TEST(path == convertable to string)    // '\\' and '/' are equal              //
//  PATH_TEST_EQ(path, path)                     // '\\' and '/' are equal              //
//                                                                                      //
//  BOOST_TEST(path.string() == path.string())   // '\\' and '/' are not equal          //
//  BOOST_TEST(path.string() ==                                                         //
//               convertable to string)          // '\\' and '/' are not equal          //
//  PATH_TEST_EQ(path.string(),                                                         //
//                convertable to string)         // '\\' and '/' are not equal          //
//                                                                                      //
//  The last of these is often what is needed, so the PATH_TEST_EQ macro is provided.   //
//  It converts its first argument to a path, and then performs a .string() on it,      //
//  eliminating much boilerplate .string() or even path(...).string() code.             //
//                                                                                      //
//  PATH_TEST_EQ(path, convertable to string)    // '\\' and '/' are not equal          //
//                                                                                      //
//--------------------------------------------------------------------------------------//

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

#include <boost/filesystem/operations.hpp>
#include <boost/utility.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <cassert>

namespace fs = boost::filesystem;
using boost::filesystem::path;

#include <boost/detail/lightweight_test.hpp>
#include <boost/detail/lightweight_main.hpp>

#ifdef BOOST_WINDOWS_API
# define BOOST_DIR_SEP "\\"
#else
# define BOOST_DIR_SEP "/"
#endif

#define PATH_TEST_EQ(a, b) check(a, b, __FILE__, __LINE__)

namespace
{
  std::string platform(BOOST_PLATFORM);

  void check(const fs::path & source,
              const std::string & expected, const char* file, int line)
  {
    if (source.string() == expected)
      return;

    std::cout << file
              << '(' << line << "): source: \"" << source.string()
              << "\" != expected: \"" << expected
              << "\"" << std::endl;

    ++::boost::detail::test_errors();
  }

  path p1("fe/fi/fo/fum");
  path p2(p1);
  path p3;
  path p4("foobar");
  path p5;

  //  exception_tests  -----------------------------------------------------------------//

  void exception_tests()
  {
    std::cout << "exception_tests..." << std::endl;
    const std::string str_1("string-1");
    boost::system::error_code ec(12345, boost::system::system_category());
    try { throw fs::filesystem_error(str_1, ec); }
    catch (const fs::filesystem_error & ex)
    {
      //std::cout << ex.what() << "*" << std::endl;
      //BOOST_TEST(std::strcmp(ex.what(),
      //  "string-1: Unknown error") == 0);
      BOOST_TEST(ex.code() == ec);
    }

    try { throw fs::filesystem_error(str_1, "p1", "p2", ec); }
    catch (const fs::filesystem_error & ex)
    {
      //std::cout << ex.what() << "*" << std::endl;                    
      //BOOST_TEST(std::strcmp(ex.what(),
      //  "string-1: Unknown error: \"p1\", \"p2\"") == 0);
      BOOST_TEST(ex.code() == ec);
      BOOST_TEST(ex.path1() == "p1");
      BOOST_TEST(ex.path2() == "p2");
    }
  }

  //  overload_tests  ------------------------------------------------------------------//

  // These verify various overloads don't cause compiler errors
  // They pre-date operations_unit_test.cpp

  void overload_tests()
  {
    std::cout << "overload_tests..." << std::endl;

    fs::exists(p1);
    fs::exists("foo");
    fs::exists(std::string("foo"));

    fs::exists(p1 / path("foo"));
    fs::exists(p1 / "foo");
    fs::exists(p1 / std::string("foo"));

    fs::exists("foo" / p1);
    fs::exists(std::string("foo") / p1);

    p4 /= path("foo");
    p4 /= "foo";
    p4 /= std::string("foo");
  }

  //  iterator_tests  ------------------------------------------------------------------//

  void iterator_tests()
  {
    std::cout << "iterator_tests..." << std::endl;

    path itr_ck = "";
    path::const_iterator itr = itr_ck.begin();
    BOOST_TEST(itr == itr_ck.end());

    itr_ck = "/";
    itr = itr_ck.begin();
    BOOST_TEST(itr->string() == "/");
    BOOST_TEST(++itr == itr_ck.end());
    BOOST_TEST((--itr)->string() == "/");

    itr_ck = "foo";
    BOOST_TEST(*itr_ck.begin() == std::string("foo"));
    BOOST_TEST(boost::next(itr_ck.begin()) == itr_ck.end());
    BOOST_TEST(*boost::prior(itr_ck.end()) == std::string("foo"));
    BOOST_TEST(boost::prior(itr_ck.end()) == itr_ck.begin());

    itr_ck = path("/foo");
    BOOST_TEST((itr_ck.begin())->string() == "/");
    BOOST_TEST(*boost::next(itr_ck.begin()) == std::string("foo"));
    BOOST_TEST(boost::next(boost::next(itr_ck.begin())) == itr_ck.end());
    BOOST_TEST(boost::next(itr_ck.begin()) == boost::prior(itr_ck.end()));
    BOOST_TEST(*boost::prior(itr_ck.end()) == std::string("foo"));
    BOOST_TEST(*boost::prior(boost::prior(itr_ck.end())) == std::string("/"));
    BOOST_TEST(boost::prior(boost::prior(itr_ck.end())) == itr_ck.begin());

    itr_ck = "/foo/bar";
    itr = itr_ck.begin();
    BOOST_TEST(itr->string() == "/");
    BOOST_TEST(*++itr == std::string("foo"));
    BOOST_TEST(*++itr == std::string("bar"));
    BOOST_TEST(++itr == itr_ck.end());
    PATH_TEST_EQ(*--itr, "bar");
    PATH_TEST_EQ(*--itr, "foo");
    PATH_TEST_EQ(*--itr, "/");

    itr_ck = "../f"; // previously failed due to short name bug
    itr = itr_ck.begin();
    PATH_TEST_EQ(itr->string(), "..");
    PATH_TEST_EQ(*++itr, "f");
    BOOST_TEST(++itr == itr_ck.end());
    PATH_TEST_EQ(*--itr, "f");
    PATH_TEST_EQ(*--itr, "..");

    // POSIX says treat "/foo/bar/" as "/foo/bar/."
    itr_ck = "/foo/bar/";
    itr = itr_ck.begin();
    PATH_TEST_EQ(itr->string(), "/");
    PATH_TEST_EQ(*++itr, "foo");
    PATH_TEST_EQ(*++itr, "bar");
    PATH_TEST_EQ(*++itr, ".");
    BOOST_TEST(++itr == itr_ck.end());
    PATH_TEST_EQ(*--itr, ".");
    PATH_TEST_EQ(*--itr, "bar");
    PATH_TEST_EQ(*--itr, "foo");
    PATH_TEST_EQ(*--itr, "/");

    // POSIX says treat "/f/b/" as "/f/b/."
    itr_ck = "/f/b/";
    itr = itr_ck.begin();
    PATH_TEST_EQ(itr->string(), "/");
    PATH_TEST_EQ(*++itr, "f");
    PATH_TEST_EQ(*++itr, "b");
    PATH_TEST_EQ(*++itr, ".");
    BOOST_TEST(++itr == itr_ck.end());
    PATH_TEST_EQ(*--itr, ".");
    PATH_TEST_EQ(*--itr, "b");
    PATH_TEST_EQ(*--itr, "f");
    PATH_TEST_EQ(*--itr, "/");

    itr_ck = "//net";
    itr = itr_ck.begin();
    // two leading slashes are permitted by POSIX (as implementation defined),
    // while for Windows it is always well defined (as a network name)
    PATH_TEST_EQ(itr->string(), "//net");
    BOOST_TEST(++itr == itr_ck.end());
    PATH_TEST_EQ(*--itr, "//net");

    itr_ck = "//net/";
    itr = itr_ck.begin();
    PATH_TEST_EQ(itr->string(), "//net");
    PATH_TEST_EQ(*++itr, "/");
    BOOST_TEST(++itr == itr_ck.end());
    PATH_TEST_EQ(*--itr, "/");
    PATH_TEST_EQ(*--itr, "//net");

    itr_ck = "//foo///bar///";
    itr = itr_ck.begin();
    PATH_TEST_EQ(itr->string(), "//foo");
    PATH_TEST_EQ(*++itr, "/");
    PATH_TEST_EQ(*++itr, "bar");
    PATH_TEST_EQ(*++itr, ".");
    BOOST_TEST(++itr == itr_ck.end());
    PATH_TEST_EQ(*--itr, ".");
    PATH_TEST_EQ(*--itr, "bar");
    PATH_TEST_EQ(*--itr, "/");
    PATH_TEST_EQ(*--itr, "//foo");

    itr_ck = "///foo///bar///";
    itr = itr_ck.begin();
    // three or more leading slashes are to be treated as a single slash
    PATH_TEST_EQ(itr->string(), "/");
    PATH_TEST_EQ(*++itr, "foo");
    PATH_TEST_EQ(*++itr, "bar");
    PATH_TEST_EQ(*++itr, ".");
    BOOST_TEST(++itr == itr_ck.end());
    PATH_TEST_EQ(*--itr, ".");
    PATH_TEST_EQ(*--itr, "bar");
    PATH_TEST_EQ(*--itr, "foo");
    PATH_TEST_EQ(*--itr, "/");

    if (platform == "Windows")
    {
      itr_ck = "c:/";
      itr = itr_ck.begin();
      PATH_TEST_EQ(itr->string(), "c:");
      PATH_TEST_EQ(*++itr, std::string("/"));
      BOOST_TEST(++itr == itr_ck.end());
      PATH_TEST_EQ(*--itr, "/");
      PATH_TEST_EQ(*--itr, "c:");

      itr_ck = "c:\\";
      itr = itr_ck.begin();
      PATH_TEST_EQ(itr->string(), "c:");
      PATH_TEST_EQ(*++itr, "/");  // test that iteration returns generic format
      BOOST_TEST(++itr == itr_ck.end());
      PATH_TEST_EQ(*--itr, "/");  // test that iteration returns generic format
      PATH_TEST_EQ(*--itr, "c:");

      itr_ck = "c:/foo";
      itr = itr_ck.begin();
      BOOST_TEST(*itr == std::string("c:"));
      BOOST_TEST(*++itr == std::string("/"));
      BOOST_TEST(*++itr == std::string("foo"));
      BOOST_TEST(++itr == itr_ck.end());
      BOOST_TEST(*--itr == std::string("foo"));
      BOOST_TEST((--itr)->string() == "/");
      BOOST_TEST(*--itr == std::string("c:"));

      itr_ck = "c:\\foo";
      itr = itr_ck.begin();
      BOOST_TEST(*itr == std::string("c:"));
      BOOST_TEST(*++itr == std::string("\\"));
      BOOST_TEST(*++itr == std::string("foo"));
      BOOST_TEST(++itr == itr_ck.end());
      BOOST_TEST(*--itr == std::string("foo"));
      BOOST_TEST(*--itr == std::string("\\"));
      BOOST_TEST(*--itr == std::string("c:"));

      itr_ck = "\\\\\\foo\\\\\\bar\\\\\\";
      itr = itr_ck.begin();
      // three or more leading slashes are to be treated as a single slash
      PATH_TEST_EQ(itr->string(), "/");
      PATH_TEST_EQ(*++itr, "foo");
      PATH_TEST_EQ(*++itr, "bar");
      PATH_TEST_EQ(*++itr, ".");
      BOOST_TEST(++itr == itr_ck.end());
      PATH_TEST_EQ(*--itr, ".");
      PATH_TEST_EQ(*--itr, "bar");
      PATH_TEST_EQ(*--itr, "foo");
      PATH_TEST_EQ(*--itr, "/");

      itr_ck = "c:foo";
      itr = itr_ck.begin();
      BOOST_TEST(*itr == std::string("c:"));
      BOOST_TEST(*++itr == std::string("foo"));
      BOOST_TEST(++itr == itr_ck.end());
      BOOST_TEST(*--itr == std::string("foo"));
      BOOST_TEST(*--itr == std::string("c:"));

      itr_ck = "c:foo/";
      itr = itr_ck.begin();
      BOOST_TEST(*itr == std::string("c:"));
      BOOST_TEST(*++itr == std::string("foo"));
      BOOST_TEST(*++itr == std::string("."));
      BOOST_TEST(++itr == itr_ck.end());
      BOOST_TEST(*--itr == std::string("."));
      BOOST_TEST(*--itr == std::string("foo"));
      BOOST_TEST(*--itr == std::string("c:"));

      itr_ck = path("c:");
      BOOST_TEST(*itr_ck.begin() == std::string("c:"));
      BOOST_TEST(next(itr_ck.begin()) == itr_ck.end());
      BOOST_TEST(prior(itr_ck.end()) == itr_ck.begin());
      BOOST_TEST(*prior(itr_ck.end()) == std::string("c:"));

      itr_ck = path("c:/");
      BOOST_TEST(*itr_ck.begin() == std::string("c:"));
      BOOST_TEST(*next(itr_ck.begin()) == std::string("/"));
      BOOST_TEST(next(next(itr_ck.begin())) == itr_ck.end());
      BOOST_TEST(prior(prior(itr_ck.end())) == itr_ck.begin());
      BOOST_TEST(*prior(itr_ck.end()) == std::string("/"));
      BOOST_TEST(*prior(prior(itr_ck.end())) == std::string("c:"));

      itr_ck = path("c:foo");
      BOOST_TEST(*itr_ck.begin() == std::string("c:"));
      BOOST_TEST(*next(itr_ck.begin()) == std::string("foo"));
      BOOST_TEST(next(next(itr_ck.begin())) == itr_ck.end());
      BOOST_TEST(prior(prior(itr_ck.end())) == itr_ck.begin());
      BOOST_TEST(*prior(itr_ck.end()) == std::string("foo"));
      BOOST_TEST(*prior(prior(itr_ck.end())) == std::string("c:"));

      itr_ck = path("c:/foo");
      BOOST_TEST(*itr_ck.begin() == std::string("c:"));
      BOOST_TEST(*next(itr_ck.begin()) == std::string("/"));
      BOOST_TEST(*next(next(itr_ck.begin())) == std::string("foo"));
      BOOST_TEST(next(next(next(itr_ck.begin()))) == itr_ck.end());
      BOOST_TEST(prior(prior(prior(itr_ck.end()))) == itr_ck.begin());
      BOOST_TEST(*prior(itr_ck.end()) == std::string("foo"));
      BOOST_TEST(*prior(prior(itr_ck.end())) == std::string("/"));
      BOOST_TEST(*prior(prior(prior(itr_ck.end()))) == std::string("c:"));

      itr_ck = path("//net");
      BOOST_TEST(*itr_ck.begin() == std::string("//net"));
      BOOST_TEST(next(itr_ck.begin()) == itr_ck.end());
      BOOST_TEST(prior(itr_ck.end()) == itr_ck.begin());
      BOOST_TEST(*prior(itr_ck.end()) == std::string("//net"));

      itr_ck = path("//net/");
      PATH_TEST_EQ(itr_ck.begin()->string(), "//net");
      PATH_TEST_EQ(next(itr_ck.begin())->string(), "/");
      BOOST_TEST(next(next(itr_ck.begin())) == itr_ck.end());
      BOOST_TEST(prior(prior(itr_ck.end())) == itr_ck.begin());
      PATH_TEST_EQ(prior(itr_ck.end())->string(), "/");
      PATH_TEST_EQ(prior(prior(itr_ck.end()))->string(), "//net");

      itr_ck = path("//net/foo");
      BOOST_TEST(*itr_ck.begin() == std::string("//net"));
      BOOST_TEST(*next(itr_ck.begin()) == std::string("/"));
      BOOST_TEST(*next(next(itr_ck.begin())) == std::string("foo"));
      BOOST_TEST(next(next(next(itr_ck.begin()))) == itr_ck.end());
      BOOST_TEST(prior(prior(prior(itr_ck.end()))) == itr_ck.begin());
      BOOST_TEST(*prior(itr_ck.end()) == std::string("foo"));
      BOOST_TEST(*prior(prior(itr_ck.end())) == std::string("/"));
      BOOST_TEST(*prior(prior(prior(itr_ck.end()))) == std::string("//net"));

      itr_ck = path("prn:");
      BOOST_TEST(*itr_ck.begin() == std::string("prn:"));
      BOOST_TEST(next(itr_ck.begin()) == itr_ck.end());
      BOOST_TEST(prior(itr_ck.end()) == itr_ck.begin());
      BOOST_TEST(*prior(itr_ck.end()) == std::string("prn:"));
    }
    else
    {
      itr_ck = "///";
      itr = itr_ck.begin();
      PATH_TEST_EQ(itr->string(),  "/");
      BOOST_TEST(++itr == itr_ck.end());
    }
  }

  //  non_member_tests  ----------------------------------------------------------------//

  void non_member_tests()
  {
    std::cout << "non_member_tests..." << std::endl;

    // test non-member functions, particularly operator overloads
                                                               
    path e, e2;
    std::string es, es2;
    char ecs[] = "";
    char ecs2[] = "";

    char acs[] = "a";
    std::string as(acs);
    path a(as);

    char acs2[] = "a";
    std::string as2(acs2);
    path a2(as2);

    char bcs[] = "b";
    std::string bs(bcs);
    path b(bs);

    // swap
    a.swap(b);
    BOOST_TEST(a.string() == "b");
    BOOST_TEST(b.string() == "a");
    fs::swap(a, b);
    BOOST_TEST(a.string() == "a");
    BOOST_TEST(b.string() == "b");

    // probe operator /
    PATH_TEST_EQ(path("") / ".", ".");
    PATH_TEST_EQ(path("") / "..", "..");
    if (platform == "Windows")
    {
      BOOST_TEST(path("foo\\bar") == "foo/bar");
      BOOST_TEST((b / a).native() == path("b\\a").native());
      BOOST_TEST((bs / a).native() == path("b\\a").native());
      BOOST_TEST((bcs / a).native() == path("b\\a").native());
      BOOST_TEST((b / as).native() == path("b\\a").native());
      BOOST_TEST((b / acs).native() == path("b\\a").native());
      PATH_TEST_EQ(path("a") / "b", "a\\b");
      PATH_TEST_EQ(path("..") / "", "..");
      PATH_TEST_EQ(path("foo") / path("bar"), "foo\\bar"); // path arg
      PATH_TEST_EQ(path("foo") / "bar", "foo\\bar");       // const char* arg
      PATH_TEST_EQ(path("foo") / path("woo/bar").filename(), "foo\\bar"); // const std::string & arg
      PATH_TEST_EQ("foo" / path("bar"), "foo\\bar");
      PATH_TEST_EQ(path("..") / ".." , "..\\..");
      PATH_TEST_EQ(path("/") / ".." , "/..");
      PATH_TEST_EQ(path("/..") / ".." , "/..\\..");
      PATH_TEST_EQ(path("..") / "foo" , "..\\foo");
      PATH_TEST_EQ(path("foo") / ".." , "foo\\..");
      PATH_TEST_EQ(path("..") / "f" , "..\\f");
      PATH_TEST_EQ(path("/..") / "f" , "/..\\f");
      PATH_TEST_EQ(path("f") / ".." , "f\\..");
      PATH_TEST_EQ(path("foo") / ".." / ".." , "foo\\..\\..");
      PATH_TEST_EQ(path("foo") / ".." / ".." / ".." , "foo\\..\\..\\..");
      PATH_TEST_EQ(path("f") / ".." / "b" , "f\\..\\b");
      PATH_TEST_EQ(path("foo") / ".." / "bar" , "foo\\..\\bar");
      PATH_TEST_EQ(path("foo") / "bar" / ".." , "foo\\bar\\..");
      PATH_TEST_EQ(path("foo") / "bar" / ".." / "..", "foo\\bar\\..\\..");
      PATH_TEST_EQ(path("foo") / "bar" / ".." / "blah", "foo\\bar\\..\\blah");
      PATH_TEST_EQ(path("f") / "b" / ".." , "f\\b\\..");
      PATH_TEST_EQ(path("f") / "b" / ".." / "a", "f\\b\\..\\a");
      PATH_TEST_EQ(path("foo") / "bar" / "blah" / ".." / "..", "foo\\bar\\blah\\..\\..");
      PATH_TEST_EQ(path("foo") / "bar" / "blah" / ".." / ".." / "bletch", "foo\\bar\\blah\\..\\..\\bletch");

      PATH_TEST_EQ(path(".") / "foo", ".\\foo");
      PATH_TEST_EQ(path(".") / "..", ".\\..");
      PATH_TEST_EQ(path("foo") / ".", "foo\\.");
      PATH_TEST_EQ(path("..") / ".", "..\\.");
      PATH_TEST_EQ(path(".") / ".", ".\\.");
      PATH_TEST_EQ(path(".") / "." / ".", ".\\.\\.");
      PATH_TEST_EQ(path(".") / "foo" / ".", ".\\foo\\.");
      PATH_TEST_EQ(path("foo") / "." / "bar", "foo\\.\\bar");
      PATH_TEST_EQ(path("foo") / "." / ".", "foo\\.\\.");
      PATH_TEST_EQ(path("foo") / "." / "..", "foo\\.\\..");
      PATH_TEST_EQ(path(".") / "." / "..", ".\\.\\..");
      PATH_TEST_EQ(path(".") / ".." / ".", ".\\..\\.");
      PATH_TEST_EQ(path("..") / "." / ".", "..\\.\\.");
    }
    else  // POSIX
    {
      PATH_TEST_EQ(b / a, "b/a");
      PATH_TEST_EQ(bs / a, "b/a");
      PATH_TEST_EQ(bcs / a, "b/a");
      PATH_TEST_EQ(b / as, "b/a");
      PATH_TEST_EQ(b / acs, "b/a");
      PATH_TEST_EQ(path("a") / "b", "a/b");
      PATH_TEST_EQ(path("..") / "", "..");
      PATH_TEST_EQ(path("") / "..", "..");
      PATH_TEST_EQ(path("foo") / path("bar"), "foo/bar"); // path arg
      PATH_TEST_EQ(path("foo") / "bar", "foo/bar");       // const char* arg
      PATH_TEST_EQ(path("foo") / path("woo/bar").filename(), "foo/bar"); // const std::string & arg
      PATH_TEST_EQ("foo" / path("bar"), "foo/bar");
      PATH_TEST_EQ(path("..") / ".." , "../..");
      PATH_TEST_EQ(path("/") / ".." , "/..");
      PATH_TEST_EQ(path("/..") / ".." , "/../..");
      PATH_TEST_EQ(path("..") / "foo" , "../foo");
      PATH_TEST_EQ(path("foo") / ".." , "foo/..");
      PATH_TEST_EQ(path("..") / "f" , "../f");
      PATH_TEST_EQ(path("/..") / "f" , "/../f");
      PATH_TEST_EQ(path("f") / ".." , "f/..");
      PATH_TEST_EQ(path("foo") / ".." / ".." , "foo/../..");
      PATH_TEST_EQ(path("foo") / ".." / ".." / ".." , "foo/../../..");
      PATH_TEST_EQ(path("f") / ".." / "b" , "f/../b");
      PATH_TEST_EQ(path("foo") / ".." / "bar" , "foo/../bar");
      PATH_TEST_EQ(path("foo") / "bar" / ".." , "foo/bar/..");
      PATH_TEST_EQ(path("foo") / "bar" / ".." / "..", "foo/bar/../..");
      PATH_TEST_EQ(path("foo") / "bar" / ".." / "blah", "foo/bar/../blah");
      PATH_TEST_EQ(path("f") / "b" / ".." , "f/b/..");
      PATH_TEST_EQ(path("f") / "b" / ".." / "a", "f/b/../a");
      PATH_TEST_EQ(path("foo") / "bar" / "blah" / ".." / "..", "foo/bar/blah/../..");
      PATH_TEST_EQ(path("foo") / "bar" / "blah" / ".." / ".." / "bletch", "foo/bar/blah/../../bletch");

      PATH_TEST_EQ(path(".") / "foo", "./foo");
      PATH_TEST_EQ(path(".") / "..", "./..");
      PATH_TEST_EQ(path("foo") / ".", "foo/.");
      PATH_TEST_EQ(path("..") / ".", "../.");
      PATH_TEST_EQ(path(".") / ".", "./.");
      PATH_TEST_EQ(path(".") / "." / ".", "././.");
      PATH_TEST_EQ(path(".") / "foo" / ".", "./foo/.");
      PATH_TEST_EQ(path("foo") / "." / "bar", "foo/./bar");
      PATH_TEST_EQ(path("foo") / "." / ".", "foo/./.");
      PATH_TEST_EQ(path("foo") / "." / "..", "foo/./..");
      PATH_TEST_EQ(path(".") / "." / "..", "././..");
      PATH_TEST_EQ(path(".") / ".." / ".", "./../.");
      PATH_TEST_EQ(path("..") / "." / ".", ".././.");
    }

    // probe operator <
    BOOST_TEST(!(e < e2));
    BOOST_TEST(!(es < e2));
    BOOST_TEST(!(ecs < e2));
    BOOST_TEST(!(e < es2));
    BOOST_TEST(!(e < ecs2));

    BOOST_TEST(e < a);
    BOOST_TEST(es < a);
    BOOST_TEST(ecs < a);
    BOOST_TEST(e < as);
    BOOST_TEST(e < acs);

    BOOST_TEST(a < b);
    BOOST_TEST(as < b);
    BOOST_TEST(acs < b);
    BOOST_TEST(a < bs);
    BOOST_TEST(a < bcs);

    BOOST_TEST(!(a < a2));
    BOOST_TEST(!(as < a2));
    BOOST_TEST(!(acs < a2));
    BOOST_TEST(!(a < as2));
    BOOST_TEST(!(a < acs2));

    // make sure basic_path overloads don't conflict with std::string overloads

    BOOST_TEST(!(as < as));
    BOOST_TEST(!(as < acs));
    BOOST_TEST(!(acs < as));

    // reality check character set is as expected
    BOOST_TEST(std::string("a.b") < std::string("a/b"));
    // verify compare is actually lexicographical
    BOOST_TEST(path("a/b") < path("a.b"));

    // make sure the derivative operators also work

    BOOST_TEST(b > a);
    BOOST_TEST(b > as);
    BOOST_TEST(b > acs);
    BOOST_TEST(bs > a);
    BOOST_TEST(bcs > a);

    BOOST_TEST(!(a2 > a));
    BOOST_TEST(!(a2 > as));
    BOOST_TEST(!(a2 > acs));
    BOOST_TEST(!(as2 > a));
    BOOST_TEST(!(acs2 > a));

    BOOST_TEST(a <= b);
    BOOST_TEST(as <= b);
    BOOST_TEST(acs <= b);
    BOOST_TEST(a <= bs);
    BOOST_TEST(a <= bcs);

    BOOST_TEST(a <= a2);
    BOOST_TEST(as <= a2);
    BOOST_TEST(acs <= a2);
    BOOST_TEST(a <= as2);
    BOOST_TEST(a <= acs2);

    BOOST_TEST(b >= a);
    BOOST_TEST(bs >= a);
    BOOST_TEST(bcs >= a);
    BOOST_TEST(b >= as);
    BOOST_TEST(b >= acs);

    BOOST_TEST(a2 >= a);
    BOOST_TEST(as2 >= a);
    BOOST_TEST(acs2 >= a);
    BOOST_TEST(a2 >= as);
    BOOST_TEST(a2 >= acs);

    //  operator == and != are implemented separately, so test separately

    path p1("fe/fi/fo/fum");
    path p2(p1);
    path p3("fe/fi/fo/fumm");
    BOOST_TEST(p1.string() != p3.string());

    // check each overload
    BOOST_TEST(p1 != p3);
    BOOST_TEST(p1 != p3.string());
    BOOST_TEST(p1 != p3.string().c_str());
    BOOST_TEST(p1.string() != p3);
    BOOST_TEST(p1.string().c_str() != p3);

    p3 = p2;
    BOOST_TEST(p1.string() == p3.string());

    // check each overload
    BOOST_TEST(p1 == p3);
    BOOST_TEST(p1 == p3.string());
    BOOST_TEST(p1 == p3.string().c_str());
    BOOST_TEST(p1.string() == p3);
    BOOST_TEST(p1.string().c_str() == p3);

    if (platform == "Windows")
    {
      std::cout << "Windows relational tests..." << std::endl;
      path p10 ("c:\\file");
      path p11 ("c:/file");
      // check each overload
      BOOST_TEST(p10.generic_string() == p11.generic_string());
      BOOST_TEST(p10 == p11);
      BOOST_TEST(p10 == p11.string());
      BOOST_TEST(p10 == p11.string().c_str());
      BOOST_TEST(p10.string() == p11);
      BOOST_TEST(p10.string().c_str() == p11);
      BOOST_TEST(p10 == L"c:\\file");
      BOOST_TEST(p10 == L"c:/file");
      BOOST_TEST(p11 == L"c:\\file");
      BOOST_TEST(p11 == L"c:/file");
      BOOST_TEST(L"c:\\file" == p10);
      BOOST_TEST(L"c:/file" == p10);
      BOOST_TEST(L"c:\\file" == p11);
      BOOST_TEST(L"c:/file" == p11);

      BOOST_TEST(!(p10.generic_string() != p11.generic_string()));
      BOOST_TEST(!(p10 != p11));
      BOOST_TEST(!(p10 != p11.string()));
      BOOST_TEST(!(p10 != p11.string().c_str()));
      BOOST_TEST(!(p10.string() != p11));
      BOOST_TEST(!(p10.string().c_str() != p11));
      BOOST_TEST(!(p10 != L"c:\\file"));
      BOOST_TEST(!(p10 != L"c:/file"));
      BOOST_TEST(!(p11 != L"c:\\file"));
      BOOST_TEST(!(p11 != L"c:/file"));
      BOOST_TEST(!(L"c:\\file" != p10));
      BOOST_TEST(!(L"c:/file" != p10));
      BOOST_TEST(!(L"c:\\file" != p11));
      BOOST_TEST(!(L"c:/file" != p11));

      BOOST_TEST(!(p10.string() < p11.string()));
      BOOST_TEST(!(p10 < p11));
      BOOST_TEST(!(p10 < p11.string()));
      BOOST_TEST(!(p10 < p11.string().c_str()));
      BOOST_TEST(!(p10.string() < p11));
      BOOST_TEST(!(p10.string().c_str() < p11));
      BOOST_TEST(!(p10 < L"c:\\file"));
      BOOST_TEST(!(p10 < L"c:/file"));
      BOOST_TEST(!(p11 < L"c:\\file"));
      BOOST_TEST(!(p11 < L"c:/file"));
      BOOST_TEST(!(L"c:\\file" < p10));
      BOOST_TEST(!(L"c:/file" < p10));
      BOOST_TEST(!(L"c:\\file" < p11));
      BOOST_TEST(!(L"c:/file" < p11));

      BOOST_TEST(!(p10.generic_string() > p11.generic_string()));
      BOOST_TEST(!(p10 > p11));
      BOOST_TEST(!(p10 > p11.string()));
      BOOST_TEST(!(p10 > p11.string().c_str()));
      BOOST_TEST(!(p10.string() > p11));
      BOOST_TEST(!(p10.string().c_str() > p11));
      BOOST_TEST(!(p10 > L"c:\\file"));
      BOOST_TEST(!(p10 > L"c:/file"));
      BOOST_TEST(!(p11 > L"c:\\file"));
      BOOST_TEST(!(p11 > L"c:/file"));
      BOOST_TEST(!(L"c:\\file" > p10));
      BOOST_TEST(!(L"c:/file" > p10));
      BOOST_TEST(!(L"c:\\file" > p11));
      BOOST_TEST(!(L"c:/file" > p11));
    }
  }

  //  query_and_decomposition_tests  ---------------------------------------------------//
  //
  //  remove_filename() is also tested here, because its specification depends on
  //  a decomposition function.

  void query_and_decomposition_tests()
  {
    std::cout << "query_and_decomposition_tests..." << std::endl;

    // stem() tests not otherwise covered
    BOOST_TEST(path("b").stem() == "b");
    BOOST_TEST(path("a/b.txt").stem() == "b");
    BOOST_TEST(path("a/b.").stem() == "b"); 
    BOOST_TEST(path("a.b.c").stem() == "a.b");
    BOOST_TEST(path("a.b.c.").stem() == "a.b.c");

    // extension() tests not otherwise covered
    BOOST_TEST(path("a/b").extension() == "");
    BOOST_TEST(path("a.b/c").extension() == "");
    BOOST_TEST(path("a/b.txt").extension() == ".txt");
    BOOST_TEST(path("a/b.").extension() == ".");
    BOOST_TEST(path("a.b.c").extension() == ".c");
    BOOST_TEST(path("a.b.c.").extension() == ".");
    BOOST_TEST(path("a/").extension() == "");

    // main q & d test sequence
    path p;
    path q;

    p = q = "";
    BOOST_TEST(p.relative_path().string() == "");
    BOOST_TEST(p.parent_path().string() == "");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    BOOST_TEST(p.filename() == "");
    BOOST_TEST(p.stem() == "");
    BOOST_TEST(p.extension() == "");
    BOOST_TEST(p.root_name() == "");
    BOOST_TEST(p.root_directory() == "");
    BOOST_TEST(p.root_path().string() == "");
    BOOST_TEST(!p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(!p.has_root_directory());
    BOOST_TEST(!p.has_relative_path());
    BOOST_TEST(!p.has_filename());
    BOOST_TEST(!p.has_stem());
    BOOST_TEST(!p.has_extension());
    BOOST_TEST(!p.has_parent_path());
    BOOST_TEST(!p.is_absolute());

    p = q = "/";
    BOOST_TEST(p.relative_path().string() == "");
    BOOST_TEST(p.parent_path().string() == "");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    BOOST_TEST(p.filename() == "/");
    BOOST_TEST(p.stem() == "/");
    BOOST_TEST(p.extension() == "");
    BOOST_TEST(p.root_name() == "");
    BOOST_TEST(p.root_directory() == "/");
    BOOST_TEST(p.root_path().string() == "/");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(p.has_root_directory());
    BOOST_TEST(!p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_stem());
    BOOST_TEST(!p.has_extension());
    BOOST_TEST(!p.has_parent_path());
    if (platform == "POSIX")
      BOOST_TEST(p.is_absolute());
    else
      BOOST_TEST(!p.is_absolute());

    p = q = "//";
    PATH_TEST_EQ(p.relative_path().string(), "");
    PATH_TEST_EQ(p.parent_path().string(), "");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    PATH_TEST_EQ(p.filename(), "//");
    PATH_TEST_EQ(p.stem(), "//");
    PATH_TEST_EQ(p.extension(), "");
    PATH_TEST_EQ(p.root_name(), "//");
    PATH_TEST_EQ(p.root_directory(), "");
    PATH_TEST_EQ(p.root_path().string(), "//");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(p.has_root_name());
    BOOST_TEST(!p.has_root_directory());
    BOOST_TEST(!p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_stem());
    BOOST_TEST(!p.has_extension());
    BOOST_TEST(!p.has_parent_path());
    BOOST_TEST(!p.is_absolute());

    p = q = "///";
    PATH_TEST_EQ(p.relative_path().string(), "");
    PATH_TEST_EQ(p.parent_path().string(), "");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    PATH_TEST_EQ(p.filename(), "/");
    PATH_TEST_EQ(p.stem(), "/");
    PATH_TEST_EQ(p.extension(), "");
    PATH_TEST_EQ(p.root_name(), "");
    PATH_TEST_EQ(p.root_directory(), "/");
    PATH_TEST_EQ(p.root_path().string(), "/");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(p.has_root_directory());
    BOOST_TEST(!p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_stem());
    BOOST_TEST(!p.has_extension());
    BOOST_TEST(!p.has_parent_path());
    if (platform == "POSIX")
      BOOST_TEST(p.is_absolute());
    else
      BOOST_TEST(!p.is_absolute());

    p = q = ".";
    BOOST_TEST(p.relative_path().string() == ".");
    BOOST_TEST(p.parent_path().string() == "");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    BOOST_TEST(p.filename() == ".");
    BOOST_TEST(p.stem() == ".");
    BOOST_TEST(p.extension() == "");
    BOOST_TEST(p.root_name() == "");
    BOOST_TEST(p.root_directory() == "");
    BOOST_TEST(p.root_path().string() == "");
    BOOST_TEST(!p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(!p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_stem());
    BOOST_TEST(!p.has_extension());
    BOOST_TEST(!p.has_parent_path());
    BOOST_TEST(!p.is_absolute());

    p = q = "..";
    BOOST_TEST(p.relative_path().string() == "..");
    BOOST_TEST(p.parent_path().string() == "");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    BOOST_TEST(p.filename() == "..");
    BOOST_TEST(p.stem() == "..");
    BOOST_TEST(p.extension() == "");
    BOOST_TEST(p.root_name() == "");
    BOOST_TEST(p.root_directory() == "");
    BOOST_TEST(p.root_path().string() == "");
    BOOST_TEST(!p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(!p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_stem());
    BOOST_TEST(!p.has_extension());
    BOOST_TEST(!p.has_parent_path());
    BOOST_TEST(!p.is_absolute());

    p = q = "foo";
    BOOST_TEST(p.relative_path().string() == "foo");
    BOOST_TEST(p.parent_path().string() == "");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    BOOST_TEST(p.filename() == "foo");
    BOOST_TEST(p.stem() == "foo");
    BOOST_TEST(p.extension() == "");
    BOOST_TEST(p.root_name() == "");
    BOOST_TEST(p.root_directory() == "");
    BOOST_TEST(p.root_path().string() == "");
    BOOST_TEST(!p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(!p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_stem());
    BOOST_TEST(!p.has_extension());
    BOOST_TEST(!p.has_parent_path());
    BOOST_TEST(!p.is_absolute());

    p = q = "/foo";
    PATH_TEST_EQ(p.relative_path().string(), "foo");
    PATH_TEST_EQ(p.parent_path().string(), "/");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    PATH_TEST_EQ(p.filename(), "foo");
    PATH_TEST_EQ(p.stem(), "foo");
    PATH_TEST_EQ(p.extension(), "");
    PATH_TEST_EQ(p.root_name(), "");
    PATH_TEST_EQ(p.root_directory(), "/");
    PATH_TEST_EQ(p.root_path().string(), "/");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_stem());
    BOOST_TEST(!p.has_extension());
    BOOST_TEST(p.has_parent_path());
    if (platform == "POSIX")
      BOOST_TEST(p.is_absolute());
    else
      BOOST_TEST(!p.is_absolute());

    p = q = "/foo/";
    PATH_TEST_EQ(p.relative_path().string(), "foo/");
    PATH_TEST_EQ(p.parent_path().string(), "/foo");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    PATH_TEST_EQ(p.filename(), ".");
    PATH_TEST_EQ(p.stem(), ".");
    PATH_TEST_EQ(p.extension(), "");
    PATH_TEST_EQ(p.root_name(), "");
    PATH_TEST_EQ(p.root_directory(), "/");
    PATH_TEST_EQ(p.root_path().string(), "/");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_stem());
    BOOST_TEST(!p.has_extension());
    BOOST_TEST(p.has_parent_path());
    if (platform == "POSIX")
      BOOST_TEST(p.is_absolute());
    else
      BOOST_TEST(!p.is_absolute());

    p = q = "///foo";
    PATH_TEST_EQ(p.relative_path().string(), "foo");
    PATH_TEST_EQ(p.parent_path().string(), "/");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    PATH_TEST_EQ(p.filename(), "foo");
    PATH_TEST_EQ(p.root_name(), "");
    PATH_TEST_EQ(p.root_directory(), "/");
    PATH_TEST_EQ(p.root_path().string(), "/");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_parent_path());
    if (platform == "POSIX")
      BOOST_TEST(p.is_absolute());
    else
      BOOST_TEST(!p.is_absolute());

    p = q = "foo/bar";
    BOOST_TEST(p.relative_path().string() == "foo/bar");
    BOOST_TEST(p.parent_path().string() == "foo");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    BOOST_TEST(p.filename() == "bar");
    BOOST_TEST(p.stem() == "bar");
    BOOST_TEST(p.extension() == "");
    BOOST_TEST(p.root_name() == "");
    BOOST_TEST(p.root_directory() == "");
    BOOST_TEST(p.root_path().string() == "");
    BOOST_TEST(!p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(!p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_stem());
    BOOST_TEST(!p.has_extension());
    BOOST_TEST(p.has_parent_path());
    BOOST_TEST(!p.is_absolute());

    p = q = "../foo";
    BOOST_TEST(p.relative_path().string() == "../foo");
    BOOST_TEST(p.parent_path().string() == "..");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    BOOST_TEST(p.filename() == "foo");
    BOOST_TEST(p.root_name() == "");
    BOOST_TEST(p.root_directory() == "");
    BOOST_TEST(p.root_path().string() == "");
    BOOST_TEST(!p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(!p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_parent_path());
    BOOST_TEST(!p.is_absolute());

    p = q = "..///foo";
    PATH_TEST_EQ(p.relative_path().string(), "..///foo");
    PATH_TEST_EQ(p.parent_path().string(), "..");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    PATH_TEST_EQ(p.filename(), "foo");
    PATH_TEST_EQ(p.root_name(), "");
    PATH_TEST_EQ(p.root_directory(), "");
    PATH_TEST_EQ(p.root_path().string(), "");
    BOOST_TEST(!p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(!p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_parent_path());
    BOOST_TEST(!p.is_absolute());

    p = q = "/foo/bar";
    BOOST_TEST(p.relative_path().string() == "foo/bar");
    BOOST_TEST(p.parent_path().string() == "/foo");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    BOOST_TEST(p.filename() == "bar");
    BOOST_TEST(p.root_name() == "");
    BOOST_TEST(p.root_directory() == "/");
    BOOST_TEST(p.root_path().string() == "/");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(!p.has_root_name());
    BOOST_TEST(p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_parent_path());
    if (platform == "POSIX")
      BOOST_TEST(p.is_absolute());
    else
      BOOST_TEST(!p.is_absolute());

    // Both POSIX and Windows allow two leading slashs
    // (POSIX meaning is implementation defined)
    PATH_TEST_EQ(path("//resource"), "//resource");
    PATH_TEST_EQ(path("//resource/"), "//resource/");
    PATH_TEST_EQ(path("//resource/foo"), "//resource/foo");

    p = q = path("//net");
    PATH_TEST_EQ(p.string(), "//net");
    PATH_TEST_EQ(p.relative_path().string(), "");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    PATH_TEST_EQ(p.parent_path().string(), "");
    PATH_TEST_EQ(p.filename(), "//net");
    PATH_TEST_EQ(p.root_name(), "//net");
    PATH_TEST_EQ(p.root_directory(), "");
    PATH_TEST_EQ(p.root_path().string(), "//net");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(p.has_root_name());
    BOOST_TEST(!p.has_root_directory());
    BOOST_TEST(!p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(!p.has_parent_path());
    BOOST_TEST(!p.is_absolute());

    p = q = path("//net/");
    BOOST_TEST(p.relative_path().string() == "");
    BOOST_TEST(p.parent_path().string() == "//net");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    BOOST_TEST(p.filename() == "/");
    BOOST_TEST(p.root_name() == "//net");
    BOOST_TEST(p.root_directory() == "/");
    BOOST_TEST(p.root_path().string() == "//net/");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(p.has_root_name());
    BOOST_TEST(p.has_root_directory());
    BOOST_TEST(!p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_parent_path());
    BOOST_TEST(p.is_absolute());

    p = q = path("//net/foo");
    BOOST_TEST(p.relative_path().string() == "foo");
    BOOST_TEST(p.parent_path().string() == "//net/");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    BOOST_TEST(p.filename() == "foo");
    BOOST_TEST(p.root_name() == "//net");
    BOOST_TEST(p.root_directory() == "/");
    BOOST_TEST(p.root_path().string() == "//net/");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(p.has_root_name());
    BOOST_TEST(p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_parent_path());
    BOOST_TEST(p.is_absolute());

    p = q = path("//net///foo");
    PATH_TEST_EQ(p.relative_path().string(), "foo");
    PATH_TEST_EQ(p.parent_path().string(), "//net/");
    PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
    PATH_TEST_EQ(p.filename(), "foo");
    PATH_TEST_EQ(p.root_name(), "//net");
    PATH_TEST_EQ(p.root_directory(), "/");
    PATH_TEST_EQ(p.root_path().string(), "//net/");
    BOOST_TEST(p.has_root_path());
    BOOST_TEST(p.has_root_name());
    BOOST_TEST(p.has_root_directory());
    BOOST_TEST(p.has_relative_path());
    BOOST_TEST(p.has_filename());
    BOOST_TEST(p.has_parent_path());
    BOOST_TEST(p.is_absolute());

    if (platform == "Windows")
    {
 
      //p = q = L"\\\\?\\";
      //BOOST_TEST(p.relative_path().string() == "");
      //BOOST_TEST(p.parent_path().string() == "");
      //PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      //BOOST_TEST(p.filename() == "");
      //BOOST_TEST(p.stem() == "");
      //BOOST_TEST(p.extension() == "");
      //BOOST_TEST(p.root_name() == "");
      //BOOST_TEST(p.root_directory() == "");
      //BOOST_TEST(p.root_path().string() == "");
      //BOOST_TEST(!p.has_root_path());
      //BOOST_TEST(!p.has_root_name());
      //BOOST_TEST(!p.has_root_directory());
      //BOOST_TEST(!p.has_relative_path());
      //BOOST_TEST(!p.has_filename());
      //BOOST_TEST(!p.has_stem());
      //BOOST_TEST(!p.has_extension());
      //BOOST_TEST(!p.has_parent_path());
      //BOOST_TEST(!p.is_absolute());

      p = q = path("c:");
      BOOST_TEST(p.relative_path().string() == "");
      BOOST_TEST(p.parent_path().string() == "");
      PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      BOOST_TEST(p.filename() == "c:");
      BOOST_TEST(p.root_name() == "c:");
      BOOST_TEST(p.root_directory() == "");
      BOOST_TEST(p.root_path().string() == "c:");
      BOOST_TEST(p.has_root_path());
      BOOST_TEST(p.has_root_name());
      BOOST_TEST(!p.has_root_directory());
      BOOST_TEST(!p.has_relative_path());
      BOOST_TEST(p.has_filename());
      BOOST_TEST(!p.has_parent_path());
      BOOST_TEST(!p.is_absolute());
 
      //p = q = path(L"\\\\?\\c:");
      //BOOST_TEST(p.relative_path().string() == "");
      //BOOST_TEST(p.parent_path().string() == "");
      //PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      //BOOST_TEST(p.filename() == "c:");
      //BOOST_TEST(p.root_name() == "c:");
      //BOOST_TEST(p.root_directory() == "");
      //BOOST_TEST(p.root_path().string() == "c:");
      //BOOST_TEST(p.has_root_path());
      //BOOST_TEST(p.has_root_name());
      //BOOST_TEST(!p.has_root_directory());
      //BOOST_TEST(!p.has_relative_path());
      //BOOST_TEST(p.has_filename());
      //BOOST_TEST(!p.has_parent_path());
      //BOOST_TEST(!p.is_absolute());

      p = q = path("c:foo");
      BOOST_TEST(p.relative_path().string() == "foo");
      BOOST_TEST(p.parent_path().string() == "c:");
      PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      BOOST_TEST(p.filename() == "foo");
      BOOST_TEST(p.root_name() == "c:");
      BOOST_TEST(p.root_directory() == "");
      BOOST_TEST(p.root_path().string() == "c:");
      BOOST_TEST(p.has_root_path());
      BOOST_TEST(p.has_root_name());
      BOOST_TEST(!p.has_root_directory());
      BOOST_TEST(p.has_relative_path());
      BOOST_TEST(p.has_filename());
      BOOST_TEST(p.has_parent_path());
      BOOST_TEST(!p.is_absolute());

      //p = q = path(L"\\\\?\\c:foo");
      //BOOST_TEST(p.relative_path().string() == "foo");
      //BOOST_TEST(p.parent_path().string() == "c:");
      //PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      //BOOST_TEST(p.filename() == "foo");
      //BOOST_TEST(p.root_name() == "c:");
      //BOOST_TEST(p.root_directory() == "");
      //BOOST_TEST(p.root_path().string() == "c:");
      //BOOST_TEST(p.has_root_path());
      //BOOST_TEST(p.has_root_name());
      //BOOST_TEST(!p.has_root_directory());
      //BOOST_TEST(p.has_relative_path());
      //BOOST_TEST(p.has_filename());
      //BOOST_TEST(p.has_parent_path());
      //BOOST_TEST(!p.is_absolute());
   
      p = q = path("c:/");
      BOOST_TEST(p.relative_path().string() == "");
      BOOST_TEST(p.parent_path().string() == "c:");
      PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      BOOST_TEST(p.filename() == "/");
      BOOST_TEST(p.root_name() == "c:");
      BOOST_TEST(p.root_directory() == "/");
      BOOST_TEST(p.root_path().string() == "c:/");
      BOOST_TEST(p.has_root_path());
      BOOST_TEST(p.has_root_name());
      BOOST_TEST(p.has_root_directory());
      BOOST_TEST(!p.has_relative_path());
      BOOST_TEST(p.has_filename());
      BOOST_TEST(p.has_parent_path());
      BOOST_TEST(p.is_absolute());

      p = q = path("c:..");
      BOOST_TEST(p.relative_path().string() == "..");
      BOOST_TEST(p.parent_path().string() == "c:");
      PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      BOOST_TEST(p.filename() == "..");
      BOOST_TEST(p.root_name() == "c:");
      BOOST_TEST(p.root_directory() == "");
      BOOST_TEST(p.root_path().string() == "c:");
      BOOST_TEST(p.has_root_path());
      BOOST_TEST(p.has_root_name());
      BOOST_TEST(!p.has_root_directory());
      BOOST_TEST(p.has_relative_path());
      BOOST_TEST(p.has_filename());
      BOOST_TEST(p.has_parent_path());
      BOOST_TEST(!p.is_absolute());

      p = q = path("c:/foo");
      PATH_TEST_EQ(p.relative_path().string(), "foo");
      PATH_TEST_EQ(p.parent_path().string(), "c:/");
      PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      PATH_TEST_EQ(p.filename(), "foo");
      PATH_TEST_EQ(p.root_name(), "c:");
      PATH_TEST_EQ(p.root_directory(), "/");
      PATH_TEST_EQ(p.root_path().string(), "c:/");
      BOOST_TEST(p.has_root_path());
      BOOST_TEST(p.has_root_name());
      BOOST_TEST(p.has_root_directory());
      BOOST_TEST(p.has_relative_path());
      BOOST_TEST(p.has_filename());
      BOOST_TEST(p.has_parent_path());
      BOOST_TEST(p.is_absolute());

      p = q = path("c://foo");
      PATH_TEST_EQ(p.relative_path().string(), "foo");
      PATH_TEST_EQ(p.parent_path().string(), "c:/");
      PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      PATH_TEST_EQ(p.filename(), "foo");
      PATH_TEST_EQ(p.root_name(), "c:");
      PATH_TEST_EQ(p.root_directory(), "/");
      PATH_TEST_EQ(p.root_path().string(), "c:/");
      BOOST_TEST(p.has_root_path());
      BOOST_TEST(p.has_root_name());
      BOOST_TEST(p.has_root_directory());
      BOOST_TEST(p.has_relative_path());
      BOOST_TEST(p.has_filename());
      BOOST_TEST(p.has_parent_path());
      BOOST_TEST(p.is_absolute());

      p = q = path("c:\\foo\\bar");
      PATH_TEST_EQ(p.relative_path().string(), "foo\\bar");
      PATH_TEST_EQ(p.parent_path().string(), "c:\\foo");
      PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      PATH_TEST_EQ(p.filename(), "bar");
      PATH_TEST_EQ(p.root_name(), "c:");
      PATH_TEST_EQ(p.root_directory(), "\\");
      PATH_TEST_EQ(p.root_path().string(), "c:\\");
      BOOST_TEST(p.has_root_path());
      BOOST_TEST(p.has_root_name());
      BOOST_TEST(p.has_root_directory());
      BOOST_TEST(p.has_relative_path());
      BOOST_TEST(p.has_filename());
      BOOST_TEST(p.has_parent_path());
      BOOST_TEST(p.is_absolute());

      p = q = path("prn:");
      BOOST_TEST(p.relative_path().string() == "");
      BOOST_TEST(p.parent_path().string() == "");
      PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      BOOST_TEST(p.filename() == "prn:");
      BOOST_TEST(p.root_name() == "prn:");
      BOOST_TEST(p.root_directory() == "");
      BOOST_TEST(p.root_path().string() == "prn:");
      BOOST_TEST(p.has_root_path());
      BOOST_TEST(p.has_root_name());
      BOOST_TEST(!p.has_root_directory());
      BOOST_TEST(!p.has_relative_path());
      BOOST_TEST(p.has_filename());
      BOOST_TEST(!p.has_parent_path());
      BOOST_TEST(!p.is_absolute());

      p = q = path("\\\\net\\\\\\foo");
      PATH_TEST_EQ(p.relative_path().string(), "foo");
      PATH_TEST_EQ(p.parent_path().string(), "\\\\net\\");
      PATH_TEST_EQ(q.remove_filename().string(), p.parent_path().string());
      PATH_TEST_EQ(p.filename(), "foo");
      PATH_TEST_EQ(p.root_name(), "\\\\net");
      PATH_TEST_EQ(p.root_directory(), "\\");
      PATH_TEST_EQ(p.root_path().string(), "\\\\net\\");
      BOOST_TEST(p.has_root_path());
      BOOST_TEST(p.has_root_name());
      BOOST_TEST(p.has_root_directory());
      BOOST_TEST(p.has_relative_path());
      BOOST_TEST(p.has_filename());
      BOOST_TEST(p.has_parent_path());
      BOOST_TEST(p.is_absolute());
    } // Windows

    else
    { // POSIX
      PATH_TEST_EQ(path("/foo/bar/"), "/foo/bar/");
      PATH_TEST_EQ(path("//foo//bar//"), "//foo//bar//");
      PATH_TEST_EQ(path("///foo///bar///"), "///foo///bar///");

      p = path("/usr/local/bin:/usr/bin:/bin");
      BOOST_TEST(p.string() == "/usr/local/bin:/usr/bin:/bin");
    } // POSIX
  }

 //  composition_tests  ----------------------------------------------------------------//

  void composition_tests()
  {
    std::cout << "composition_tests..." << std::endl;

  }

 //  construction_tests  ---------------------------------------------------------------//

  void construction_tests()
  {
    std::cout << "construction_tests..." << std::endl;

    PATH_TEST_EQ("", "");

    PATH_TEST_EQ("foo", "foo");
    PATH_TEST_EQ("f", "f");

    PATH_TEST_EQ("foo/", "foo/");
    PATH_TEST_EQ("f/", "f/");
    PATH_TEST_EQ("foo/..", "foo/..");
    PATH_TEST_EQ("foo/../", "foo/../");
    PATH_TEST_EQ("foo/bar/../..", "foo/bar/../..");
    PATH_TEST_EQ("foo/bar/../../", "foo/bar/../../");
    PATH_TEST_EQ("/", "/");
    PATH_TEST_EQ("/f", "/f");

    PATH_TEST_EQ("/foo", "/foo");
    PATH_TEST_EQ("/foo/bar/", "/foo/bar/");
    PATH_TEST_EQ("//foo//bar//", "//foo//bar//");
    PATH_TEST_EQ("///foo///bar///", "///foo///bar///");
    PATH_TEST_EQ("\\/foo\\/bar\\/", "\\/foo\\/bar\\/");
    PATH_TEST_EQ("\\//foo\\//bar\\//", "\\//foo\\//bar\\//");

    if (platform == "Windows")
    {
      PATH_TEST_EQ(path("c:") / "foo", "c:foo");
      PATH_TEST_EQ(path("c:") / "/foo", "c:/foo");

      PATH_TEST_EQ("\\foo\\bar\\", "\\foo\\bar\\");
      PATH_TEST_EQ("\\\\foo\\\\bar\\\\", "\\\\foo\\\\bar\\\\");
      PATH_TEST_EQ("\\\\\\foo\\\\\\bar\\\\\\", "\\\\\\foo\\\\\\bar\\\\\\");

      PATH_TEST_EQ("\\", "\\");
      PATH_TEST_EQ("\\f", "\\f");
      PATH_TEST_EQ("\\foo", "\\foo");
      PATH_TEST_EQ("foo\\bar", "foo\\bar");
      PATH_TEST_EQ("foo bar", "foo bar");
      PATH_TEST_EQ("c:", "c:");
      PATH_TEST_EQ("c:/", "c:/");
      PATH_TEST_EQ("c:.", "c:.");
      PATH_TEST_EQ("c:./foo", "c:./foo");
      PATH_TEST_EQ("c:.\\foo", "c:.\\foo");
      PATH_TEST_EQ("c:..", "c:..");
      PATH_TEST_EQ("c:/.", "c:/.");
      PATH_TEST_EQ("c:/..", "c:/..");
      PATH_TEST_EQ("c:/../", "c:/../");
      PATH_TEST_EQ("c:\\..\\", "c:\\..\\");
      PATH_TEST_EQ("c:/../..", "c:/../..");
      PATH_TEST_EQ("c:/../foo", "c:/../foo");
      PATH_TEST_EQ("c:\\..\\foo", "c:\\..\\foo");
      PATH_TEST_EQ("c:../foo", "c:../foo");
      PATH_TEST_EQ("c:..\\foo", "c:..\\foo");
      PATH_TEST_EQ("c:/../../foo", "c:/../../foo");
      PATH_TEST_EQ("c:\\..\\..\\foo", "c:\\..\\..\\foo");
      PATH_TEST_EQ("c:foo/..", "c:foo/..");
      PATH_TEST_EQ("c:/foo/..", "c:/foo/..");
      PATH_TEST_EQ("c:/..foo", "c:/..foo");
      PATH_TEST_EQ("c:foo", "c:foo");
      PATH_TEST_EQ("c:/foo", "c:/foo");
      PATH_TEST_EQ("\\\\netname", "\\\\netname");
      PATH_TEST_EQ("\\\\netname\\", "\\\\netname\\");
      PATH_TEST_EQ("\\\\netname\\foo", "\\\\netname\\foo");
      PATH_TEST_EQ("c:/foo", "c:/foo");
      PATH_TEST_EQ("prn:", "prn:");
    }
    else
    {
    }

    PATH_TEST_EQ("foo/bar", "foo/bar");
    PATH_TEST_EQ("a/b", "a/b");  // probe for length effects
    PATH_TEST_EQ("..", "..");
    PATH_TEST_EQ("../..", "../..");
    PATH_TEST_EQ("/..", "/..");
    PATH_TEST_EQ("/../..", "/../..");
    PATH_TEST_EQ("../foo", "../foo");
    PATH_TEST_EQ("foo/..", "foo/..");
    PATH_TEST_EQ("foo/..bar", "foo/..bar");
    PATH_TEST_EQ("../f", "../f");
    PATH_TEST_EQ("/../f", "/../f");
    PATH_TEST_EQ("f/..", "f/..");
    PATH_TEST_EQ("foo/../..", "foo/../..");
    PATH_TEST_EQ("foo/../../..", "foo/../../..");
    PATH_TEST_EQ("foo/../bar", "foo/../bar");
    PATH_TEST_EQ("foo/bar/..", "foo/bar/..");
    PATH_TEST_EQ("foo/bar/../..", "foo/bar/../..");
    PATH_TEST_EQ("foo/bar/../blah", "foo/bar/../blah");
    PATH_TEST_EQ("f/../b", "f/../b");
    PATH_TEST_EQ("f/b/..", "f/b/..");
    PATH_TEST_EQ("f/b/../a", "f/b/../a");
    PATH_TEST_EQ("foo/bar/blah/../..", "foo/bar/blah/../..");
    PATH_TEST_EQ("foo/bar/blah/../../bletch", "foo/bar/blah/../../bletch");
    PATH_TEST_EQ("...", "...");
    PATH_TEST_EQ("....", "....");
    PATH_TEST_EQ("foo/...", "foo/...");
    PATH_TEST_EQ("abc.", "abc.");
    PATH_TEST_EQ("abc..", "abc..");
    PATH_TEST_EQ("foo/abc.", "foo/abc.");
    PATH_TEST_EQ("foo/abc..", "foo/abc..");

    PATH_TEST_EQ(".abc", ".abc");
    PATH_TEST_EQ("a.c", "a.c");
    PATH_TEST_EQ("..abc", "..abc");
    PATH_TEST_EQ("a..c", "a..c");
    PATH_TEST_EQ("foo/.abc", "foo/.abc");
    PATH_TEST_EQ("foo/a.c", "foo/a.c");
    PATH_TEST_EQ("foo/..abc", "foo/..abc");
    PATH_TEST_EQ("foo/a..c", "foo/a..c");

    PATH_TEST_EQ(".", ".");
    PATH_TEST_EQ("./foo", "./foo");
    PATH_TEST_EQ("./..", "./..");
    PATH_TEST_EQ("./../foo", "./../foo");
    PATH_TEST_EQ("foo/.", "foo/.");
    PATH_TEST_EQ("../.", "../.");
    PATH_TEST_EQ("./.", "./.");
    PATH_TEST_EQ("././.", "././.");
    PATH_TEST_EQ("./foo/.", "./foo/.");
    PATH_TEST_EQ("foo/./bar", "foo/./bar");
    PATH_TEST_EQ("foo/./.", "foo/./.");
    PATH_TEST_EQ("foo/./..", "foo/./..");
    PATH_TEST_EQ("foo/./../bar", "foo/./../bar");
    PATH_TEST_EQ("foo/../.", "foo/../.");
    PATH_TEST_EQ("././..", "././..");
    PATH_TEST_EQ("./../.", "./../.");
    PATH_TEST_EQ(".././.", ".././.");
  }

  //  append_tests  --------------------------------------------------------------------//

  void append_test_aux(const path & p, const std::string & s, const std::string & expect)
  {
    PATH_TEST_EQ((p / path(s)).string(), expect);
    PATH_TEST_EQ((p / s.c_str()).string(), expect);
    PATH_TEST_EQ((p / s).string(), expect);
    path x(p);
    x.append(s.begin(), s.end());
    PATH_TEST_EQ(x.string(), expect);
  }

  void append_tests()
  {
    std::cout << "append_tests..." << std::endl;

    // There are many control paths to be exercised, since empty paths and arguments,
    // paths with trailing separators, arguments with leading separators, with or without
    // other characters being present, are all separate cases that need to be tested.
    // Furthermore, some of the code to be tested is specific to argument categories,
    // so that results in further permutations to be tested.

    //// code to generate test cases
    //// 
    //// expected results must be checked by hand
    //// "foo\bar" expected result must be edited by hand and moved for Windows/POSIX
    ////
    //const char* x[]    = { "", "/", "foo", "foo/" };
    //const char* y[] = { "", "/", "bar", "/bar" };

    //for (int i = 0; i < sizeof(x)/sizeof(char*); ++i)
    //  for (int j = 0; j < sizeof(y)/sizeof(char*); ++j)
    //  {
    //    std::cout << "\n    PATH_TEST_EQ(path(\"" << x[i] << "\") / \"" << y[j] << "\", \"" 
    //              << path(x[i]) / y[j] << "\");\n";
    //    std::cout << "    append_test_aux(\"" << x[i] << "\", \"" << y[j] << "\", \""
    //              << path(x[i]) / y[j] << "\");\n";
    //  }

    PATH_TEST_EQ(path("") / "", "");
    append_test_aux("", "", "");

    PATH_TEST_EQ(path("") / "/", "/");
    append_test_aux("", "/", "/");

    PATH_TEST_EQ(path("") / "bar", "bar");
    append_test_aux("", "bar", "bar");

    PATH_TEST_EQ(path("") / "/bar", "/bar");
    append_test_aux("", "/bar", "/bar");

    PATH_TEST_EQ(path("/") / "", "/");
    append_test_aux("/", "", "/");

    PATH_TEST_EQ(path("/") / "/", "//");
    append_test_aux("/", "/", "//");

    PATH_TEST_EQ(path("/") / "bar", "/bar");
    append_test_aux("/", "bar", "/bar");

    PATH_TEST_EQ(path("/") / "/bar", "//bar");
    append_test_aux("/", "/bar", "//bar");

    PATH_TEST_EQ(path("foo") / "", "foo");
    append_test_aux("foo", "", "foo");

    PATH_TEST_EQ(path("foo") / "/", "foo/");
    append_test_aux("foo", "/", "foo/");

    PATH_TEST_EQ(path("foo") / "/bar", "foo/bar");
    append_test_aux("foo", "/bar", "foo/bar");

    PATH_TEST_EQ(path("foo/") / "", "foo/");
    append_test_aux("foo/", "", "foo/");

    PATH_TEST_EQ(path("foo/") / "/", "foo//");
    append_test_aux("foo/", "/", "foo//");

    PATH_TEST_EQ(path("foo/") / "bar", "foo/bar");
    append_test_aux("foo/", "bar", "foo/bar");

    PATH_TEST_EQ(path("foo/") / "/bar", "foo//bar");
    append_test_aux("foo/", "/bar", "foo//bar");

    if (platform == "Windows")
    {
      PATH_TEST_EQ(path("foo") / "bar", "foo\\bar");
      append_test_aux("foo", "bar", "foo\\bar");

      // hand created test case specific to Windows
      PATH_TEST_EQ(path("c:") / "bar", "c:bar");
      append_test_aux("c:", "bar", "c:bar");
    }
    else
    {
      PATH_TEST_EQ(path("foo") / "bar", "foo/bar");
      append_test_aux("foo", "bar", "foo/bar");
    }

  }

//  self_assign_and_append_tests  ------------------------------------------------------//

  void self_assign_and_append_tests()
  {
    std::cout << "self_assign_and_append_tests..." << std::endl;

    path p;

    p = "snafubar";
    PATH_TEST_EQ(p = p, "snafubar");

    p = "snafubar";
    p = p.c_str();
    PATH_TEST_EQ(p, "snafubar");

    p = "snafubar";
    p.assign(p.c_str(), path::codecvt());
    PATH_TEST_EQ(p, "snafubar");  

    p = "snafubar";
    PATH_TEST_EQ(p = p.c_str()+5, "bar");

    p = "snafubar";
    PATH_TEST_EQ(p.assign(p.c_str() + 5, p.c_str() + 7), "ba");

    p = "snafubar";
    p /= p;
    PATH_TEST_EQ(p, "snafubar" BOOST_DIR_SEP "snafubar");

    p = "snafubar";
    p /= p.c_str();
    PATH_TEST_EQ(p, "snafubar" BOOST_DIR_SEP "snafubar");  

    p = "snafubar";
    p.append(p.c_str(), path::codecvt());
    PATH_TEST_EQ(p, "snafubar" BOOST_DIR_SEP "snafubar"); 

    p = "snafubar";
    PATH_TEST_EQ(p.append(p.c_str() + 5, p.c_str() + 7), "snafubar" BOOST_DIR_SEP "ba");
  }


  //  name_function_tests  -------------------------------------------------------------//

  void name_function_tests()
  {
    std::cout << "name_function_tests..." << std::endl;

    BOOST_TEST(fs::portable_posix_name(std::string("x")));
    BOOST_TEST(fs::windows_name(std::string("x")));
    BOOST_TEST(fs::portable_name(std::string("x")));
    BOOST_TEST(fs::portable_directory_name(std::string("x")));
    BOOST_TEST(fs::portable_file_name(std::string("x")));

    BOOST_TEST(fs::portable_posix_name(std::string(".")));
    BOOST_TEST(fs::windows_name(std::string(".")));
    BOOST_TEST(fs::portable_name(std::string(".")));
    BOOST_TEST(fs::portable_directory_name(std::string(".")));
    BOOST_TEST(!fs::portable_file_name(std::string(".")));

    BOOST_TEST(fs::portable_posix_name(std::string("..")));
    BOOST_TEST(fs::windows_name(std::string("..")));
    BOOST_TEST(fs::portable_name(std::string("..")));
    BOOST_TEST(fs::portable_directory_name(std::string("..")));
    BOOST_TEST(!fs::portable_file_name(std::string("..")));

    BOOST_TEST(!fs::native(std::string("")));
    BOOST_TEST(!fs::portable_posix_name(std::string("")));
    BOOST_TEST(!fs::windows_name(std::string("")));
    BOOST_TEST(!fs::portable_name(std::string("")));
    BOOST_TEST(!fs::portable_directory_name(std::string("")));
    BOOST_TEST(!fs::portable_file_name(std::string("")));

    BOOST_TEST(!fs::native(std::string(" ")));
    BOOST_TEST(!fs::portable_posix_name(std::string(" ")));
    BOOST_TEST(!fs::windows_name(std::string(" ")));
    BOOST_TEST(!fs::portable_name(std::string(" ")));
    BOOST_TEST(!fs::portable_directory_name(std::string(" ")));
    BOOST_TEST(!fs::portable_file_name(std::string(" ")));

    BOOST_TEST(!fs::portable_posix_name(std::string(":")));
    BOOST_TEST(!fs::windows_name(std::string(":")));
    BOOST_TEST(!fs::portable_name(std::string(":")));
    BOOST_TEST(!fs::portable_directory_name(std::string(":")));
    BOOST_TEST(!fs::portable_file_name(std::string(":")));

    BOOST_TEST(fs::portable_posix_name(std::string("-")));
    BOOST_TEST(fs::windows_name(std::string("-")));
    BOOST_TEST(!fs::portable_name(std::string("-")));
    BOOST_TEST(!fs::portable_directory_name(std::string("-")));
    BOOST_TEST(!fs::portable_file_name(std::string("-")));

    BOOST_TEST(!fs::portable_posix_name(std::string("foo bar")));
    BOOST_TEST(fs::windows_name(std::string("foo bar")));
    BOOST_TEST(!fs::windows_name(std::string(" bar")));
    BOOST_TEST(!fs::windows_name(std::string("foo ")));
    BOOST_TEST(!fs::portable_name(std::string("foo bar")));
    BOOST_TEST(!fs::portable_directory_name(std::string("foo bar")));
    BOOST_TEST(!fs::portable_file_name(std::string("foo bar")));

    BOOST_TEST(fs::portable_posix_name(std::string("foo.bar")));
    BOOST_TEST(fs::windows_name(std::string("foo.bar")));
    BOOST_TEST(fs::portable_name(std::string("foo.bar")));
    BOOST_TEST(!fs::portable_directory_name(std::string("foo.bar")));
    BOOST_TEST(fs::portable_file_name(std::string("foo.bar")));

    BOOST_TEST(fs::portable_posix_name(std::string("foo.barf")));
    BOOST_TEST(fs::windows_name(std::string("foo.barf")));
    BOOST_TEST(fs::portable_name(std::string("foo.barf")));
    BOOST_TEST(!fs::portable_directory_name(std::string("foo.barf")));
    BOOST_TEST(!fs::portable_file_name(std::string("foo.barf")));

    BOOST_TEST(fs::portable_posix_name(std::string(".foo")));
    BOOST_TEST(fs::windows_name(std::string(".foo")));
    BOOST_TEST(!fs::portable_name(std::string(".foo")));
    BOOST_TEST(!fs::portable_directory_name(std::string(".foo")));
    BOOST_TEST(!fs::portable_file_name(std::string(".foo")));

    BOOST_TEST(fs::portable_posix_name(std::string("foo.")));
    BOOST_TEST(!fs::windows_name(std::string("foo.")));
    BOOST_TEST(!fs::portable_name(std::string("foo.")));
    BOOST_TEST(!fs::portable_directory_name(std::string("foo.")));
    BOOST_TEST(!fs::portable_file_name(std::string("foo.")));
  }
  
  //  replace_extension_tests  ---------------------------------------------------------//

  void replace_extension_tests()
  {
    std::cout << "replace_extension_tests..." << std::endl;

    BOOST_TEST(path().replace_extension().empty());
    BOOST_TEST(path().replace_extension("a").empty());
    BOOST_TEST(path().replace_extension("a.") == ".");
    BOOST_TEST(path().replace_extension("a.txt") == ".txt");
    // see the rationale in html docs for explanation why this works:
    BOOST_TEST(path().replace_extension(".txt") == ".txt");

    BOOST_TEST(path("a.txt").replace_extension() == "a");
    BOOST_TEST(path("a.txt").replace_extension("") == "a");
    BOOST_TEST(path("a.txt").replace_extension(".") == "a.");
    BOOST_TEST(path("a.txt").replace_extension(".tex") == "a.tex");
    BOOST_TEST(path("a.txt").replace_extension("tex") == "a");
    BOOST_TEST(path("a.").replace_extension(".tex") == "a.tex");
    BOOST_TEST(path("a.").replace_extension("tex") == "a");
    BOOST_TEST(path("a").replace_extension(".txt") == "a.txt");
    BOOST_TEST(path("a").replace_extension("txt") == "a");
    BOOST_TEST(path("a.b.txt").replace_extension(".tex") == "a.b.tex");  
    BOOST_TEST(path("a.b.txt").replace_extension("tex") == "a.b");
    BOOST_TEST(path("a/b").replace_extension(".c") == "a/b.c");
    PATH_TEST_EQ(path("a.txt/b").replace_extension(".c"), "a.txt/b.c"); // ticket 4702
  }
  
  //  make_preferred_tests  ------------------------------------------------------------//

  void make_preferred_tests()
  {
    std::cout << "make_preferred_tests..." << std::endl;

    if (platform == "Windows")
    {
      BOOST_TEST(path("//abc\\def/ghi").make_preferred().native()
        == path("\\\\abc\\def\\ghi").native());
    }
    else
    {
      BOOST_TEST(path("//abc\\def/ghi").make_preferred().native()
        == path("//abc\\def/ghi").native());
    }
  }

} // unnamed namespace

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                     main                                             //
//                                                                                      //
//--------------------------------------------------------------------------------------//

int cpp_main(int, char*[])
{
  // The choice of platform is make at runtime rather than compile-time
  // so that compile errors for all platforms will be detected even though
  // only the current platform is runtime tested.
  platform = (platform == "Win32" || platform == "Win64" || platform == "Cygwin")
               ? "Windows"
               : "POSIX";
  std::cout << "Platform is " << platform << '\n';

  BOOST_TEST(p1.string() != p3.string());
  p3 = p2;
  BOOST_TEST(p1.string() == p3.string());

  path p4("foobar");
  BOOST_TEST(p4.string() == "foobar");
  p4 = p4; // self-assignment
  BOOST_TEST(p4.string() == "foobar");

  construction_tests();
  append_tests();
  self_assign_and_append_tests();
  overload_tests();
  query_and_decomposition_tests();
  composition_tests();
  iterator_tests();
  non_member_tests();
  exception_tests();
  name_function_tests();
  replace_extension_tests();
  make_preferred_tests();

  // verify deprecated names still available

# ifndef BOOST_FILESYSTEM_NO_DEPRECATED

  p1.branch_path();
  p1.leaf();
  path p_remove_leaf;
  p_remove_leaf.remove_leaf();

# endif

  std::string s1("//:somestring");  // this used to be treated specially

  // check the path member templates
  p5.assign(s1.begin(), s1.end());

  PATH_TEST_EQ(p5.string(), "//:somestring");
  p5 = s1;
  PATH_TEST_EQ(p5.string(), "//:somestring");

  // this code, courtesy of David Whetstone, detects a now fixed bug that
  // derefereced the end iterator (assuming debug build with checked itors)
  std::vector<char> v1;
  p5.assign(v1.begin(), v1.end());
  std::string s2(v1.begin(), v1.end());
  PATH_TEST_EQ(p5.string(), s2);
  p5.assign(s1.begin(), s1.begin() + 1);
  PATH_TEST_EQ(p5.string(), "/");

  BOOST_TEST(p1 != p4);
  BOOST_TEST(p1.string() == p2.string());
  BOOST_TEST(p1.string() == p3.string());
  BOOST_TEST(path("foo").filename() == "foo");
  BOOST_TEST(path("foo").parent_path().string() == "");
  BOOST_TEST(p1.filename() == "fum");
  BOOST_TEST(p1.parent_path().string() == "fe/fi/fo");
  BOOST_TEST(path("").empty() == true);
  BOOST_TEST(path("foo").empty() == false);

  // inserter and extractor tests
# if !defined(BOOST_MSVC) || BOOST_MSVC > 1300 // bypass VC++ 7.0 and earlier
  std::cout << "\nInserter and extractor test...";
  std::stringstream ss;
  ss << fs::path("foo/bar") << std::endl;
  fs::path round_trip;
  ss >> round_trip;
  BOOST_TEST(round_trip.string() == "foo/bar");
  std::cout << round_trip.string() << "..." << round_trip << " complete\n";
# endif

  return ::boost::report_errors();
}
