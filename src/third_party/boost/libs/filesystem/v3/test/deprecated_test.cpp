//  deprecated_test program --------------------------------------------------//

//  Copyright Beman Dawes 2002
//  Copyright Vladimir Prus 2002

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

//  This test verifies that various deprecated names still work. This is
//  important to preserve existing code that uses the old names.

#define BOOST_FILESYSTEM_VERSION 3

#include <boost/config.hpp>

# if defined( BOOST_NO_STD_WSTRING )
#   error Configuration not supported: Boost.Filesystem V3 and later requires std::wstring support
# endif

#define BOOST_FILESYSTEM_DEPRECATED

#include <boost/filesystem.hpp>
#include <boost/detail/lightweight_test.hpp>
#include <boost/detail/lightweight_main.hpp>

namespace fs = boost::filesystem;
using boost::filesystem::path;

#define PATH_CHECK(a, b) check(a, b, __LINE__)

namespace
{
  std::string platform(BOOST_PLATFORM);

  void check(const fs::path & source,
              const std::string & expected, int line)
  {
    if (source.generic_string()== expected) return;

    ++::boost::detail::test_errors();

    std::cout << '(' << line << ") source.string(): \"" << source.string()
              << "\" != expected: \"" << expected
              << "\"" << std::endl;
  }

  void normalize_test()
  {
    PATH_CHECK(path("").normalize(), "");
    PATH_CHECK(path("/").normalize(), "/");
    PATH_CHECK(path("//").normalize(), "//");
    PATH_CHECK(path("///").normalize(), "/");
    PATH_CHECK(path("f").normalize(), "f");
    PATH_CHECK(path("foo").normalize(), "foo");
    PATH_CHECK(path("foo/").normalize(), "foo/.");
    PATH_CHECK(path("f/").normalize(), "f/.");
    PATH_CHECK(path("/foo").normalize(), "/foo");
    PATH_CHECK(path("foo/bar").normalize(), "foo/bar");
    PATH_CHECK(path("..").normalize(), "..");
    PATH_CHECK(path("../..").normalize(), "../..");
    PATH_CHECK(path("/..").normalize(), "/..");
    PATH_CHECK(path("/../..").normalize(), "/../..");
    PATH_CHECK(path("../foo").normalize(), "../foo");
    PATH_CHECK(path("foo/..").normalize(), ".");
    PATH_CHECK(path("foo/../").normalize(), "./.");
    PATH_CHECK((path("foo") / "..").normalize() , ".");
    PATH_CHECK(path("foo/...").normalize(), "foo/...");
    PATH_CHECK(path("foo/.../").normalize(), "foo/.../.");
    PATH_CHECK(path("foo/..bar").normalize(), "foo/..bar");
    PATH_CHECK(path("../f").normalize(), "../f");
    PATH_CHECK(path("/../f").normalize(), "/../f");
    PATH_CHECK(path("f/..").normalize(), ".");
    PATH_CHECK((path("f") / "..").normalize() , ".");
    PATH_CHECK(path("foo/../..").normalize(), "..");
    PATH_CHECK(path("foo/../../").normalize(), "../.");
    PATH_CHECK(path("foo/../../..").normalize(), "../..");
    PATH_CHECK(path("foo/../../../").normalize(), "../../.");
    PATH_CHECK(path("foo/../bar").normalize(), "bar");
    PATH_CHECK(path("foo/../bar/").normalize(), "bar/.");
    PATH_CHECK(path("foo/bar/..").normalize(), "foo");
    PATH_CHECK(path("foo/bar/../").normalize(), "foo/.");
    PATH_CHECK(path("foo/bar/../..").normalize(), ".");
    PATH_CHECK(path("foo/bar/../../").normalize(), "./.");
    PATH_CHECK(path("foo/bar/../blah").normalize(), "foo/blah");
    PATH_CHECK(path("f/../b").normalize(), "b");
    PATH_CHECK(path("f/b/..").normalize(), "f");
    PATH_CHECK(path("f/b/../").normalize(), "f/.");
    PATH_CHECK(path("f/b/../a").normalize(), "f/a");
    PATH_CHECK(path("foo/bar/blah/../..").normalize(), "foo");
    PATH_CHECK(path("foo/bar/blah/../../bletch").normalize(), "foo/bletch");
    PATH_CHECK(path("//net").normalize(), "//net");
    PATH_CHECK(path("//net/").normalize(), "//net/");
    PATH_CHECK(path("//..net").normalize(), "//..net");
    PATH_CHECK(path("//net/..").normalize(), "//net/..");
    PATH_CHECK(path("//net/foo").normalize(), "//net/foo");
    PATH_CHECK(path("//net/foo/").normalize(), "//net/foo/.");
    PATH_CHECK(path("//net/foo/..").normalize(), "//net/");
    PATH_CHECK(path("//net/foo/../").normalize(), "//net/.");

    PATH_CHECK(path("/net/foo/bar").normalize(), "/net/foo/bar");
    PATH_CHECK(path("/net/foo/bar/").normalize(), "/net/foo/bar/.");
    PATH_CHECK(path("/net/foo/..").normalize(), "/net");
    PATH_CHECK(path("/net/foo/../").normalize(), "/net/.");

    PATH_CHECK(path("//net//foo//bar").normalize(), "//net/foo/bar");
    PATH_CHECK(path("//net//foo//bar//").normalize(), "//net/foo/bar/.");
    PATH_CHECK(path("//net//foo//..").normalize(), "//net/");
    PATH_CHECK(path("//net//foo//..//").normalize(), "//net/.");

    PATH_CHECK(path("///net///foo///bar").normalize(), "/net/foo/bar");
    PATH_CHECK(path("///net///foo///bar///").normalize(), "/net/foo/bar/.");
    PATH_CHECK(path("///net///foo///..").normalize(), "/net");
    PATH_CHECK(path("///net///foo///..///").normalize(), "/net/.");

    if (platform == "Windows")
    {
      PATH_CHECK(path("c:..").normalize(), "c:..");
      PATH_CHECK(path("c:foo/..").normalize(), "c:");

      PATH_CHECK(path("c:foo/../").normalize(), "c:.");

      PATH_CHECK(path("c:/foo/..").normalize(), "c:/");
      PATH_CHECK(path("c:/foo/../").normalize(), "c:/.");
      PATH_CHECK(path("c:/..").normalize(), "c:/..");
      PATH_CHECK(path("c:/../").normalize(), "c:/../.");
      PATH_CHECK(path("c:/../..").normalize(), "c:/../..");
      PATH_CHECK(path("c:/../../").normalize(), "c:/../../.");
      PATH_CHECK(path("c:/../foo").normalize(), "c:/../foo");
      PATH_CHECK(path("c:/../foo/").normalize(), "c:/../foo/.");
      PATH_CHECK(path("c:/../../foo").normalize(), "c:/../../foo");
      PATH_CHECK(path("c:/../../foo/").normalize(), "c:/../../foo/.");
      PATH_CHECK(path("c:/..foo").normalize(), "c:/..foo");
    }
    else // POSIX
    {
      PATH_CHECK(path("c:..").normalize(), "c:..");
      PATH_CHECK(path("c:foo/..").normalize(), ".");
      PATH_CHECK(path("c:foo/../").normalize(), "./.");
      PATH_CHECK(path("c:/foo/..").normalize(), "c:");
      PATH_CHECK(path("c:/foo/../").normalize(), "c:/.");
      PATH_CHECK(path("c:/..").normalize(), ".");
      PATH_CHECK(path("c:/../").normalize(), "./.");
      PATH_CHECK(path("c:/../..").normalize(), "..");
      PATH_CHECK(path("c:/../../").normalize(), "../.");
      PATH_CHECK(path("c:/../foo").normalize(), "foo");
      PATH_CHECK(path("c:/../foo/").normalize(), "foo/.");
      PATH_CHECK(path("c:/../../foo").normalize(), "../foo");
      PATH_CHECK(path("c:/../../foo/").normalize(), "../foo/.");
      PATH_CHECK(path("c:/..foo").normalize(), "c:/..foo");
    }
  }

  //  Compile-only tests not intended to be executed -----------------------------------//

  void compile_only()
  {
    fs::path p;

    fs::initial_path<fs::path>();
    fs::initial_path<fs::wpath>();

    p.file_string();
    p.directory_string();
  }

  //  path_rename_test -----------------------------------------------------------------//

  void path_rename_test()
  {
    fs::path p("foo/bar/blah");

    BOOST_TEST_EQ(path("foo/bar/blah").remove_leaf(), "foo/bar");
    BOOST_TEST_EQ(p.leaf(), "blah");
    BOOST_TEST_EQ(p.branch_path(), "foo/bar");
    BOOST_TEST(p.has_leaf());
    BOOST_TEST(p.has_branch_path());
    BOOST_TEST(!p.is_complete());

    if (platform == "Windows")
    {
      BOOST_TEST_EQ(path("foo\\bar\\blah").remove_leaf(), "foo\\bar");
      p = "foo\\bar\\blah";
      BOOST_TEST_EQ(p.branch_path(), "foo\\bar");
    }
  }

} // unnamed namespace


//--------------------------------------------------------------------------------------//

int cpp_main(int /*argc*/, char* /*argv*/[])
{
  // The choice of platform is make at runtime rather than compile-time
  // so that compile errors for all platforms will be detected even though
  // only the current platform is runtime tested.
  platform = (platform == "Win32" || platform == "Win64" || platform == "Cygwin")
               ? "Windows"
               : "POSIX";
  std::cout << "Platform is " << platform << '\n';

  BOOST_TEST(fs::initial_path() == fs::current_path());

  //path::default_name_check(fs::no_check);

  fs::directory_entry de("foo/bar");

  de.replace_leaf("", fs::file_status(), fs::file_status());

  //de.leaf();
  //de.string();

  fs::path ng(" no-way, Jose");
  BOOST_TEST(!fs::is_regular(ng));  // verify deprecated name still works
  BOOST_TEST(!fs::symbolic_link_exists("nosuchfileordirectory"));

  path_rename_test();
  normalize_test();
 
// extension() tests ---------------------------------------------------------//

  BOOST_TEST(fs::extension("a/b") == "");
  BOOST_TEST(fs::extension("a/b.txt") == ".txt");
  BOOST_TEST(fs::extension("a/b.") == ".");
  BOOST_TEST(fs::extension("a.b.c") == ".c");
  BOOST_TEST(fs::extension("a.b.c.") == ".");
  BOOST_TEST(fs::extension("") == "");
  BOOST_TEST(fs::extension("a/") == "");
  
// basename() tests ----------------------------------------------------------//

  BOOST_TEST(fs::basename("b") == "b");
  BOOST_TEST(fs::basename("a/b.txt") == "b");
  BOOST_TEST(fs::basename("a/b.") == "b"); 
  BOOST_TEST(fs::basename("a.b.c") == "a.b");
  BOOST_TEST(fs::basename("a.b.c.") == "a.b.c");
  BOOST_TEST(fs::basename("") == "");
  
// change_extension tests ---------------------------------------------------//

  BOOST_TEST(fs::change_extension("a.txt", ".tex").string() == "a.tex");
  BOOST_TEST(fs::change_extension("a.", ".tex").string() == "a.tex");
  BOOST_TEST(fs::change_extension("a", ".txt").string() == "a.txt");
  BOOST_TEST(fs::change_extension("a.b.txt", ".tex").string() == "a.b.tex");  
  // see the rationale in html docs for explanation why this works
  BOOST_TEST(fs::change_extension("", ".png").string() == ".png");

  return ::boost::report_errors();
}
