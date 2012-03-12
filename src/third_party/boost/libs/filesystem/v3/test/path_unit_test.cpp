//  filesystem path_unit_test.cpp  ---------------------------------------------------  //

//  Copyright Beman Dawes 2008, 2009

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

//  ----------------------------------------------------------------------------------  //
//
//  The purpose of this test is to ensure that each function in the public
//  interface can be called with arguments of the appropriate types. It does
//  not attempt to verify that the full range of values for each argument
//  are processed correctly.
//
//  For full functionality tests, including probes with many different argument
//  values, see path_test.cpp and other test programs.
//
//  ----------------------------------------------------------------------------------  //

#define BOOST_FILESYSTEM_VERSION 3

#include <boost/config.hpp>

# if defined( BOOST_NO_STD_WSTRING )
#   error Configuration not supported: Boost.Filesystem V3 and later requires std::wstring support
# endif

#include <boost/config/warning_disable.hpp>

//  See deprecated_test for tests of deprecated features
#ifndef BOOST_FILESYSTEM_NO_DEPRECATED 
# define BOOST_FILESYSTEM_NO_DEPRECATED
#endif
#ifndef BOOST_SYSTEM_NO_DEPRECATED 
# define BOOST_SYSTEM_NO_DEPRECATED
#endif

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/detail/utf8_codecvt_facet.hpp>  // for imbue tests
#include "test_codecvt.hpp"                                // for codecvt arg tests
#include <boost/detail/lightweight_test.hpp>
#include <boost/detail/lightweight_main.hpp>
#include <boost/smart_ptr.hpp>  // used constructor tests
#include <boost/functional/hash.hpp>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <cstring>
#include <cwchar>
#include <locale>
#include <list>

namespace fs = boost::filesystem;
namespace bs = boost::system;
using boost::filesystem::path;
using std::cout;
using std::endl;
using std::string;
using std::wstring;

#define CHECK(x) check(x, __FILE__, __LINE__)
#define PATH_IS(a, b) check_path(a, b, __FILE__, __LINE__)
#define NATIVE_IS(p, s, ws) check_native(p, s, ws, __FILE__, __LINE__)
#define IS(a,b) check_equal(a, b, __FILE__, __LINE__)

#if defined(_MSC_VER)
# pragma warning(push) // Save warning settings.
# pragma warning(disable : 4428) // Disable universal-character-name encountered in source warning.
#endif

namespace
{

  boost::system::error_code ec;
  const boost::system::error_code ok;
  const boost::system::error_code ng(-1, boost::system::system_category());

  std::string platform(BOOST_PLATFORM);

  void check_path(const path& source,
              const wstring& expected, const char* file, int line)
  {
    if (source == expected) return;

    ++::boost::detail::test_errors();

    std::cout << file;
    std::wcout << L'(' << line << L"): source.wstring(): \""
               << source.wstring()
               << L"\" != expected: \"" << expected
               << L"\"\n" ;
  }

# ifdef BOOST_WINDOWS_API
  void check_native(const path& p,
    const string&, const wstring& expected, const char* file, int line)
# else
  void check_native(const path& p,
    const string& expected, const wstring&, const char* file, int line)
# endif
  {
    if (p.native() == expected) return;

    ++::boost::detail::test_errors();

    std::cout << file  << '(' << line << "): native() is not equal expected\n"
      "  native---: " << std::hex;
    path::string_type nat(p.native());
    for (path::string_type::const_iterator it = nat.begin(); it != nat.end(); ++it)
      std::cout << long(*it) << ' ';
    std::cout << "\n  expected-: ";
    for (path::string_type::const_iterator it = expected.begin(); it != expected.end(); ++it)
      std::cout << long(*it) << ' ';
    std::cout << std::dec << std::endl;
  }

  template< class T1, class T2 >
  void check_equal(const T1& value,
                    const T2& expected, const char* file, int line)
  {
    if (value == expected) return;

    ++::boost::detail::test_errors();

    std::cout << file;

    std::wcout << L'(' << line << L"): value: \"" << value
               << L"\" != expected: \"" << expected
               << L"\"\n" ;
  }

  void check(bool ok, const char* file, int line)
  {
    if (ok) return;

    ++::boost::detail::test_errors();

    std::cout << file << '(' << line << "): test failed\n";
  }

  string s("string");
  wstring ws(L"wstring");
  std::list<char> l;      // see main() for initialization to s, t, r, i, n, g
  std::list<wchar_t> wl;  // see main() for initialization to w, s, t, r, i, n, g
  std::vector<char> v;      // see main() for initialization to f, u, z
  std::vector<wchar_t> wv;  // see main() for initialization to w, f, u, z

  class Base {};
  class Derived : public Base {};
  void fun(const boost::filesystem::path&) {}
  void fun(const boost::shared_ptr< Base >&) {}

  //  test_constructors  ---------------------------------------------------------------//

  void test_constructors()
  {
    std::cout << "testing constructors..." << std::endl;

    path x0;                                           // default constructor
    PATH_IS(x0, L"");
    BOOST_TEST_EQ(x0.native().size(), 0U);

    path x1(l.begin(), l.end());                       // iterator range char
    PATH_IS(x1, L"string");
    BOOST_TEST_EQ(x1.native().size(), 6U);

    path x2(x1);                                       // copy constructor
    PATH_IS(x2, L"string");
    BOOST_TEST_EQ(x2.native().size(), 6U);

    path x3(wl.begin(), wl.end());                     // iterator range wchar_t
    PATH_IS(x3, L"wstring");
    BOOST_TEST_EQ(x3.native().size(), 7U);

    // contiguous containers
    path x4(string("std::string"));                    // std::string
    PATH_IS(x4, L"std::string");
    BOOST_TEST_EQ(x4.native().size(), 11U);

    path x5(wstring(L"std::wstring"));                 // std::wstring
    PATH_IS(x5, L"std::wstring");
    BOOST_TEST_EQ(x5.native().size(), 12U);

    path x4v(v);                                       // std::vector<char>
    PATH_IS(x4v, L"fuz");
    BOOST_TEST_EQ(x4v.native().size(), 3U);

    path x5v(wv);                                      // std::vector<wchar_t>
    PATH_IS(x5v, L"wfuz");
    BOOST_TEST_EQ(x5v.native().size(), 4U);

    path x6("array char");                             // array char
    PATH_IS(x6, L"array char");
    BOOST_TEST_EQ(x6.native().size(), 10U);

    path x7(L"array wchar_t");                         // array wchar_t
    PATH_IS(x7, L"array wchar_t");
    BOOST_TEST_EQ(x7.native().size(), 13U);

    char char_array[100];
    std::strcpy(char_array, "big array char");
    path x6o(char_array);                              // array char, only partially full
    PATH_IS(x6o, L"big array char");
    BOOST_TEST_EQ(x6o.native().size(), 14U);

    wchar_t wchar_array[100];
    std::wcscpy(wchar_array, L"big array wchar_t");
    path x7o(wchar_array);                             // array char, only partially full
    PATH_IS(x7o, L"big array wchar_t");
    BOOST_TEST_EQ(x7o.native().size(), 17U);

    path x8(s.c_str());                                // const char* null terminated
    PATH_IS(x8, L"string");
    BOOST_TEST_EQ(x8.native().size(), 6U);

    path x9(ws.c_str());                               // const wchar_t* null terminated
    PATH_IS(x9, L"wstring");
    BOOST_TEST_EQ(x9.native().size(), 7U);

    // non-contiguous containers
    path x10(l);                                       // std::list<char>
    PATH_IS(x10, L"string");
    BOOST_TEST_EQ(x10.native().size(), 6U);

    path xll(wl);                                      // std::list<wchar_t>
    PATH_IS(xll, L"wstring");
    BOOST_TEST_EQ(xll.native().size(), 7U);

    // easy-to-make coding errors
    // path e1(x0, path::codecvt());  // fails to compile, and that is OK

    boost::shared_ptr< Derived > pDerived( new Derived() ); 
    fun( pDerived );  // tests constructor member template enable_if working correctly;
                      // will fail to compile if enable_if not taking path off the table
  }

  path x;
  path y;

  //  test_assignments  ----------------------------------------------------------------//

  void test_assignments()
  {
    std::cout << "testing assignments..." << std::endl;

    x = path("yet another path");                      // another path
    PATH_IS(x, L"yet another path");
    BOOST_TEST_EQ(x.native().size(), 16U);

    x = x;                                             // self-assignment
    PATH_IS(x, L"yet another path");
    BOOST_TEST_EQ(x.native().size(), 16U);

    x.assign(l.begin(), l.end());                      // iterator range char
    PATH_IS(x, L"string");

    x.assign(wl.begin(), wl.end());                    // iterator range wchar_t
    PATH_IS(x, L"wstring");

    x = string("std::string");                         // container char
    PATH_IS(x, L"std::string");

    x = wstring(L"std::wstring");                      // container wchar_t
    PATH_IS(x, L"std::wstring");

    x = "array char";                                  // array char
    PATH_IS(x, L"array char");

    x = L"array wchar";                                // array wchar_t
    PATH_IS(x, L"array wchar");

    x = s.c_str();                                     // const char* null terminated
    PATH_IS(x, L"string");

    x = ws.c_str();                                    // const wchar_t* null terminated
    PATH_IS(x, L"wstring");
   }

  //  test_appends  --------------------------------------------------------------------//

  void test_appends()
  {
    std::cout << "testing appends..." << std::endl;

# ifdef BOOST_WINDOWS_API
#   define BOOST_FS_FOO L"/foo\\"
# else   // POSIX paths
#   define BOOST_FS_FOO L"/foo/"
# endif

    x = "/foo";
    x /= path("");                                      // empty path
    PATH_IS(x, L"/foo");

    x = "/foo";
    x /= path("/");                                     // slash path
    PATH_IS(x, L"/foo/");

    x = "/foo";
    x /= path("/boo");                                  // slash path
    PATH_IS(x, L"/foo/boo");

    x = "/foo";
    x /= x;                                             // self-append
    PATH_IS(x, L"/foo/foo");

    x = "/foo";
    x /= path("yet another path");                      // another path
    PATH_IS(x, BOOST_FS_FOO L"yet another path");

    x = "/foo";
    x.append(l.begin(), l.end());                      // iterator range char
    PATH_IS(x, BOOST_FS_FOO L"string");

    x = "/foo";
    x.append(wl.begin(), wl.end());                    // iterator range wchar_t
    PATH_IS(x, BOOST_FS_FOO L"wstring");

    x = "/foo";
    x /= string("std::string");                         // container char
    PATH_IS(x, BOOST_FS_FOO L"std::string");

    x = "/foo";
    x /= wstring(L"std::wstring");                      // container wchar_t
    PATH_IS(x, BOOST_FS_FOO L"std::wstring");

    x = "/foo";
    x /= "array char";                                  // array char
    PATH_IS(x, BOOST_FS_FOO L"array char");

    x = "/foo";
    x /= L"array wchar";                                // array wchar_t
    PATH_IS(x, BOOST_FS_FOO L"array wchar");

    x = "/foo";
    x /= s.c_str();                                     // const char* null terminated
    PATH_IS(x, BOOST_FS_FOO L"string");

    x = "/foo";
    x /= ws.c_str();                                    // const wchar_t* null terminated
    PATH_IS(x, BOOST_FS_FOO L"wstring");
   }

  //  test_observers  ------------------------------------------------------------------//

  void test_observers()
  {
    std::cout << "testing observers..." << std::endl;

    path p0("abc");

    CHECK(p0.native().size() == 3);
    CHECK(p0.string() == "abc");
    CHECK(p0.string().size() == 3);
    CHECK(p0.wstring() == L"abc");
    CHECK(p0.wstring().size() == 3);

# ifdef BOOST_WINDOWS_API

    path p("abc\\def/ghi");

    CHECK(std::wstring(p.c_str()) == L"abc\\def/ghi");

    CHECK(p.string() == "abc\\def/ghi");
    CHECK(p.wstring() == L"abc\\def/ghi");

    CHECK(p.generic_string() == "abc/def/ghi");
    CHECK(p.generic_wstring() == L"abc/def/ghi");

    CHECK(p.generic_string<string>() == "abc/def/ghi");
    CHECK(p.generic_string<wstring>() == L"abc/def/ghi");
    CHECK(p.generic_string<path::string_type>() == L"abc/def/ghi");

# else  // BOOST_POSIX_API

    path p("abc\\def/ghi");

    CHECK(string(p.c_str()) == "abc\\def/ghi");

    CHECK(p.string() == "abc\\def/ghi");
    CHECK(p.wstring() == L"abc\\def/ghi");

    CHECK(p.generic_string() == "abc\\def/ghi");
    CHECK(p.generic_wstring() == L"abc\\def/ghi");

    CHECK(p.generic_string<string>() == "abc\\def/ghi");
    CHECK(p.generic_string<wstring>() == L"abc\\def/ghi");
    CHECK(p.generic_string<path::string_type>() == "abc\\def/ghi");

# endif
  }

  //  test_relationals  ----------------------------------------------------------------//

  void test_relationals()
  {
    std::cout << "testing relationals..." << std::endl;

    boost::hash<path> hash;

# ifdef BOOST_WINDOWS_API
    // this is a critical use case to meet user expectations
    CHECK(path("c:\\abc") == path("c:/abc"));
    CHECK(hash(path("c:\\abc")) == hash(path("c:/abc")));
# endif

    const path p("bar");
    const path p2("baz");

    CHECK(!(p < p));
    CHECK(p < p2);
    CHECK(!(p2 < p));
    CHECK(p < "baz");
    CHECK(p < string("baz"));
    CHECK(p < L"baz");
    CHECK(p < wstring(L"baz"));
    CHECK(!("baz" < p));
    CHECK(!(string("baz") < p));
    CHECK(!(L"baz" < p));
    CHECK(!(wstring(L"baz") < p));

    CHECK(p == p);
    CHECK(!(p == p2));
    CHECK(!(p2 == p));
    CHECK(p2 == "baz");
    CHECK(p2 == string("baz"));
    CHECK(p2 == L"baz");
    CHECK(p2 == wstring(L"baz"));
    CHECK("baz" == p2);
    CHECK(string("baz") == p2);
    CHECK(L"baz" == p2);
    CHECK(wstring(L"baz") == p2);

    CHECK(hash(p) == hash(p));
    CHECK(hash(p) != hash(p2)); // Not strictly required, but desirable

    CHECK(!(p != p));
    CHECK(p != p2);
    CHECK(p2 != p);

    CHECK(p <= p);
    CHECK(p <= p2);
    CHECK(!(p2 <= p));

    CHECK(!(p > p));
    CHECK(!(p > p2));
    CHECK(p2 > p);

    CHECK(p >= p);
    CHECK(!(p >= p2));
    CHECK(p2 >= p);
}

  //  test_inserter_and_extractor  -----------------------------------------------------//

  void test_inserter_and_extractor()
  {
    std::cout << "testing inserter and extractor..." << std::endl;

    path p1("foo bar");  // verify space in path roundtrips per ticket #3863
    path p2;

    std::stringstream ss;

    CHECK(p1 != p2);
    ss << p1;
    ss >> p2;
    CHECK(p1 == p2);

    path wp1(L"foo bar");
    path wp2;

    std::wstringstream wss;

    CHECK(wp1 != wp2);
    wss << wp1;
    wss >> wp2;
    CHECK(wp1 == wp2);
  }

  //  test_other_non_members  ----------------------------------------------------------//

  void test_other_non_members()
  {
    std::cout << "testing other_non_members..." << std::endl;

    path p1("foo");
    path p2("bar");

    //  operator /

    CHECK(p1 / p2 == path("foo/bar").make_preferred());
    CHECK("foo" / p2 == path("foo/bar").make_preferred());
    CHECK(L"foo" / p2 == path("foo/bar").make_preferred());
    CHECK(string("foo") / p2 == path("foo/bar").make_preferred());
    CHECK(wstring(L"foo") / p2 == path("foo/bar").make_preferred());
    CHECK(p1 / "bar" == path("foo/bar").make_preferred());
    CHECK(p1 / L"bar" == path("foo/bar").make_preferred());
    CHECK(p1 / string("bar") == path("foo/bar").make_preferred());
    CHECK(p1 / wstring(L"bar") == path("foo/bar").make_preferred());

    swap(p1, p2);

    CHECK(p1 == "bar");
    CHECK(p2 == "foo");

    CHECK(path("").remove_filename() == "");
    CHECK(path("foo").remove_filename() == "");
    CHECK(path("foo/bar").remove_filename() == "foo");
  }

//  //  test_modifiers  ------------------------------------------------------------------//
//
//  void test_modifiers()
//  {
//    std::cout << "testing modifiers..." << std::endl;
//
//  }

  //  test_iterators  ------------------------------------------------------------------//

  void test_iterators()
  {
    std::cout << "testing iterators..." << std::endl;

    path p1;
    CHECK(p1.begin() == p1.end());

    path p2("/");
    CHECK(p2.begin() != p2.end());
    CHECK(*p2.begin() == "/");
    CHECK(++p2.begin() == p2.end());

    path p3("foo/bar/baz");

    path::iterator it(p3.begin());
    CHECK(p3.begin() != p3.end());
    CHECK(*it == "foo");
    CHECK(*++it == "bar");
    CHECK(*++it == "baz");
    CHECK(*--it == "bar");
    CHECK(*--it == "foo");
    CHECK(*++it == "bar");
    CHECK(*++it == "baz");
    CHECK(++it == p3.end());
  }

  //  test_modifiers  ------------------------------------------------------------------//

  void test_modifiers()
  {
    std::cout << "testing modifiers..." << std::endl;

  }

  //  test_decompositions  -------------------------------------------------------------//

  void test_decompositions()
  {
    std::cout << "testing decompositions..." << std::endl;

    CHECK(path("").root_name().string() == "");
    CHECK(path("foo").root_name().string() == "");
    CHECK(path("/").root_name().string() == "");
    CHECK(path("/foo").root_name().string() == "");
    CHECK(path("//netname").root_name().string() == "//netname");
    CHECK(path("//netname/foo").root_name().string() == "//netname");

    CHECK(path("").root_directory().string() == "");
    CHECK(path("foo").root_directory().string() == "");
    CHECK(path("/").root_directory().string() == "/");
    CHECK(path("/foo").root_directory().string() == "/");
    CHECK(path("//netname").root_directory().string() == "");
    CHECK(path("//netname/foo").root_directory().string() == "/");

    CHECK(path("").root_path().string() == "");
    CHECK(path("/").root_path().string() == "/");
    CHECK(path("/foo").root_path().string() == "/");
    CHECK(path("//netname").root_path().string() == "//netname");
    CHECK(path("//netname/foo").root_path().string() == "//netname/");

#   ifdef BOOST_WINDOWS_API
    CHECK(path("c:/foo").root_path().string() == "c:/");
#   endif

    CHECK(path("").relative_path().string() == "");
    CHECK(path("/").relative_path().string() == "");
    CHECK(path("/foo").relative_path().string() == "foo");

    CHECK(path("").parent_path().string() == "");
    CHECK(path("/").parent_path().string() == "");
    CHECK(path("/foo").parent_path().string() == "/");
    CHECK(path("/foo/bar").parent_path().string() == "/foo");

    CHECK(path("/foo/bar/baz.zoo").filename().string() == "baz.zoo");

    CHECK(path("/foo/bar/baz.zoo").stem().string() == "baz");
    CHECK(path("/foo/bar.woo/baz").stem().string() == "baz");

    CHECK(path("foo.bar.baz.tar.bz2").extension().string() == ".bz2");
    CHECK(path("/foo/bar/baz.zoo").extension().string() == ".zoo");
    CHECK(path("/foo/bar.woo/baz").extension().string() == "");
  }

  //  test_queries  --------------------------------------------------------------------//

  void test_queries()
  {
    std::cout << "testing queries..." << std::endl;

    path p1("");
    path p2("//netname/foo.doo");

    CHECK(p1.empty());
    CHECK(!p1.has_root_path());
    CHECK(!p1.has_root_name());
    CHECK(!p1.has_root_directory());
    CHECK(!p1.has_relative_path());
    CHECK(!p1.has_parent_path());
    CHECK(!p1.has_filename());
    CHECK(!p1.has_stem());
    CHECK(!p1.has_extension());
    CHECK(!p1.is_absolute());
    CHECK(p1.is_relative());

    CHECK(!p2.empty());
    CHECK(p2.has_root_path());
    CHECK(p2.has_root_name());
    CHECK(p2.has_root_directory());
    CHECK(p2.has_relative_path());
    CHECK(p2.has_parent_path());
    CHECK(p2.has_filename());
    CHECK(p2.has_stem());
    CHECK(p2.has_extension());
    CHECK(p2.is_absolute());
    CHECK(!p2.is_relative());

  }

  //  test_imbue_locale  ---------------------------------------------------------------//

  void test_imbue_locale()
  {
    std::cout << "testing imbue locale..." << std::endl;

    //  weak test case for before/after states since we don't know what characters the
    //  default locale accepts. 
    path before("abc");

    //  So that tests are run with known encoding, use Boost UTF-8 codecvt
    //  \u2722 and \xE2\x9C\xA2 are UTF-16 and UTF-8 FOUR TEARDROP-SPOKED ASTERISK

    std::locale global_loc = std::locale();
    std::locale loc(global_loc, new fs::detail::utf8_codecvt_facet);
    std::cout << "  imbuing locale ..." << std::endl;
    std::locale old_loc = path::imbue(loc);

    std::cout << "  testing with the imbued locale ..." << std::endl;
    path p2("\xE2\x9C\xA2");
    CHECK(p2 == L"\u2722");
    CHECK(p2.wstring() == L"\u2722");

    std::cout << "  imbuing the original locale ..." << std::endl;
    path::imbue(old_loc);

    std::cout << "  testing with the original locale ..." << std::endl;
    path after("abc");
    CHECK(before == after);

    std::cout << "  locale testing complete" << std::endl;
  }

  //  test_codecvt_argument  -----------------------------------------------------------//

  void test_codecvt_argument()
  {
    std::cout << "testing codecvt arguments..." << std::endl;

    const char * c1 = "a1";
    const std::string s1(c1);
    const std::wstring ws1(L"b2");  // off-by-one mimics test_codecvt
    const std::string s2("y8");
    const std::wstring ws2(L"z9");

    test_codecvt cvt;  // produces off-by-one values that will always differ from
                       // the system's default locale codecvt facet

    int t = 0;

    //  constructors
    std::cout << "  constructors test " << ++t << std::endl;
    path p(c1, cvt);
    NATIVE_IS(p, s1, ws1);

    std::cout << "  test " << ++t << std::endl;
    path p1(s1.begin(), s1.end(), cvt);
    NATIVE_IS(p1, s1, ws1);

    std::cout << "  test " << ++t << std::endl;
    path p2(ws2, cvt);
    NATIVE_IS(p2, s2, ws2);

    std::cout << "  test " << ++t << std::endl;
    path p3(ws2.begin(), ws2.end(), cvt);
    NATIVE_IS(p3, s2, ws2);

    // path p2(p1, cvt);  // fails to compile, and that is OK

    //  assigns
    p1.clear();
    std::cout << "  assigns test " << ++t << std::endl;
    p1.assign(s1,cvt);
    NATIVE_IS(p1, s1, ws1);
    p1.clear();
    std::cout << "  test " << ++t << std::endl;
    p1.assign(s1.begin(), s1.end(), cvt);
    NATIVE_IS(p1, s1, ws1);
    // p1.assign(p, cvt);  // fails to compile, and that is OK

    //  appends
    p1.clear();
    std::cout << "  appends test " << ++t << std::endl;
    p1.append(s1,cvt);
    NATIVE_IS(p1, s1, ws1);
    p1.clear();
    std::cout << "  test " << ++t << std::endl;
    p1.append(s1.begin(), s1.end(), cvt);
    NATIVE_IS(p1, s1, ws1);
    // p1.append(p, cvt);  // fails to compile, and that is OK

    //  native observers
    std::cout << "  native observers test " << ++t << std::endl;
    CHECK(p.string<std::string>(cvt) == s1);
    std::cout << "  test " << ++t << std::endl;
    CHECK(p.string(cvt) == s1);
    std::cout << "  test " << ++t << std::endl;
    CHECK(p.string<std::wstring>(cvt) == ws1);
    std::cout << "  test " << ++t << std::endl;
    CHECK(p.wstring(cvt) == ws1);

    //  generic observers
    std::cout << "  generic observers test " << ++t << std::endl;
    CHECK(p.generic_string<std::string>(cvt) == s1);
    std::cout << "  test " << ++t << std::endl;
    CHECK(p.generic_string(cvt) == s1);
    std::cout << "  test " << ++t << std::endl;
    CHECK(p.generic_string<std::wstring>(cvt) == ws1);
    std::cout << "  test " << ++t << std::endl;
    CHECK(p.generic_wstring(cvt) == ws1);

    std::cout << "  codecvt arguments testing complete" << std::endl;
  }

  //  test_overloads  ------------------------------------------------------------------//

  void test_overloads()
  {
    std::cout << "testing overloads..." << std::endl;
    std::string s("hello");
    const char a[] = "goodbye";
    path p1(s);
    path p2(s.c_str());
    path p3(a);
    path p4("foo");

    std::wstring ws(L"hello");
    const wchar_t wa[] = L"goodbye";
    path wp1(ws);
    path wp2(ws.c_str());
    path wp3(wa);
    path wp4(L"foo");
  }

  //  test_error_handling  -------------------------------------------------------------//

  class error_codecvt
    : public std::codecvt< wchar_t, char, std::mbstate_t >
  {
  public:
    explicit error_codecvt()
        : std::codecvt<wchar_t, char, std::mbstate_t>() {}
  protected:

    virtual bool do_always_noconv() const throw() { return false; }
    virtual int do_encoding() const throw() { return 0; }

    virtual std::codecvt_base::result do_in(std::mbstate_t&,
      const char*, const char*, const char*&,
      wchar_t*, wchar_t*, wchar_t*&) const
    {
      static std::codecvt_base::result r = std::codecvt_base::noconv;
      if (r == std::codecvt_base::partial) r = std::codecvt_base::error;
      else if (r == std::codecvt_base::error) r = std::codecvt_base::noconv;
      else r = std::codecvt_base::partial;
      return r;
    }

    virtual std::codecvt_base::result do_out(std::mbstate_t &,
      const wchar_t*, const wchar_t*, const wchar_t*&,
      char*, char*, char*&) const
    {
      static std::codecvt_base::result r = std::codecvt_base::noconv;
      if (r == std::codecvt_base::partial) r = std::codecvt_base::error;
      else if (r == std::codecvt_base::error) r = std::codecvt_base::noconv;
      else r = std::codecvt_base::partial;
      return r;
    }

    virtual std::codecvt_base::result do_unshift(std::mbstate_t&,
        char*, char*, char* &) const  { return ok; }
    virtual int do_length(std::mbstate_t &,
      const char*, const char*, std::size_t) const  { return 0; }
    virtual int do_max_length() const throw () { return 0; }
  };

  void test_error_handling()
  {
    std::cout << "testing error handling..." << std::endl;

    std::locale global_loc = std::locale();
    std::locale loc(global_loc, new error_codecvt);
    std::cout << "  imbuing error locale ..." << std::endl;
    std::locale old_loc = path::imbue(loc);

    //  These tests rely on a path constructor that fails in the locale conversion.
    //  Thus construction has to call codecvt. Force that by using a narrow string
    //  for Windows, and a wide string for POSIX.
#   ifdef BOOST_WINDOWS_API
#     define STRING_FOO_ "foo"
#   else
#     define STRING_FOO_ L"foo"
#   endif

    {
      std::cout << "    testing std::codecvt_base::partial error..." << std::endl;
      bool exception_thrown (false);
      try { path(STRING_FOO_); }
      catch (const bs::system_error & ex)
      {
        exception_thrown = true;
        BOOST_TEST_EQ(ex.code(), bs::error_code(std::codecvt_base::partial,
          fs::codecvt_error_category()));
      }
      catch (...) { std::cout << "***** unexpected exception type *****" << std::endl; }
      BOOST_TEST(exception_thrown);
    }

    {
      std::cout << "    testing std::codecvt_base::error error..." << std::endl;
      bool exception_thrown (false);
      try { path(STRING_FOO_); }
      catch (const bs::system_error & ex)
      {
        exception_thrown = true;
        BOOST_TEST_EQ(ex.code(), bs::error_code(std::codecvt_base::error,
          fs::codecvt_error_category()));
      }
      catch (...) { std::cout << "***** unexpected exception type *****" << std::endl; }
      BOOST_TEST(exception_thrown);
    }

    {
      std::cout << "    testing std::codecvt_base::noconv error..." << std::endl;
      bool exception_thrown (false);
      try { path(STRING_FOO_); }
      catch (const bs::system_error & ex)
      {
        exception_thrown = true;
        BOOST_TEST_EQ(ex.code(), bs::error_code(std::codecvt_base::noconv,
          fs::codecvt_error_category()));
      }
      catch (...) { std::cout << "***** unexpected exception type *****" << std::endl; }
      BOOST_TEST(exception_thrown);
    }

    std::cout << "  restoring original locale ..." << std::endl;
    path::imbue(old_loc);
    std::cout << "  testing error handling complete" << std::endl;
 }

# if 0

//  //  test_locales  --------------------------------------------------------------------//
//
//  void test_locales()
//  {
//    std::cout << "testing locales..." << std::endl;
//
//  }

  //  test_user_supplied_type  ---------------------------------------------------------//

  typedef std::basic_string<int> user_string;

}  // unnamed namespace

namespace boost
{
namespace filesystem
{
  namespace path_traits
  {
    template<> struct is_iterator<const user_string::value_type *> { static const bool value = true; };
    template<> struct is_iterator<user_string::value_type *> { static const bool value = true; };
    template<> struct is_iterator<user_string::iterator> { static const bool value = true; };
    template<> struct is_iterator<user_string::const_iterator> { static const bool value = true; };
    template<> struct is_container<user_string> { static const bool value = true; };

    template<>
    void append<user_string::value_type>(const user_string::value_type * begin,
      const user_string::value_type * end, string_type & target, system::error_code & ec)
    {
      for (; begin != end && *begin; ++begin)
        target += *begin + 1;  // change so that results distinguishable from char cvts
    }

#  ifdef __GNUC__
    //  This specialization shouldn't be needed, and VC++, Intel, and others work
    //  fine without it. But gcc 4.3.2, and presumably other versions, need it.
    template<>
    void append<user_string::value_type>(const user_string::value_type * begin,
      string_type & target, system::error_code & ec)
    {
      path_traits::append<user_string::value_type>(begin,
        static_cast<const user_string::value_type *>(0), target, ec);
    }
#  endif

    template<>
    user_string convert<user_string>(const string_type & source,
      system::error_code & ec)
    {
      user_string temp;
      for (string_type::const_iterator it = source.begin();
            it != source.end(); ++it)
        temp += *it - 1;
      return temp;
    }
  }  // namespace path_traits
}  // namespace filesystem
}  // namespace boost

namespace
{

  void test_user_supplied_type()
  {
    std::cout << "testing user supplied type..." << std::endl;

    user_string::value_type usr_c_str[] = { 'a', 'b', 'c', 0 };
    user_string usr(usr_c_str);

    path p1(usr.c_str());
    CHECK(p1 == path("bcd"));
    CHECK(p1 == "bcd");
    user_string s1(p1.string<user_string>());
    CHECK(s1 == usr);
  }

# endif

}  // unnamed namespace

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                     main                                             //
//                                                                                      //
//--------------------------------------------------------------------------------------//

int cpp_main(int, char*[])
{
// document state of critical macros
#ifdef BOOST_POSIX_API
  cout << "BOOST_POSIX_API" << endl;
#endif
#ifdef BOOST_WINDOWS_API
  cout << "BOOST_WINDOWS_API" << endl;
#endif

  l.push_back('s');
  l.push_back('t');
  l.push_back('r');
  l.push_back('i');
  l.push_back('n');
  l.push_back('g');

  wl.push_back(L'w');
  wl.push_back(L's');
  wl.push_back(L't');
  wl.push_back(L'r');
  wl.push_back(L'i');
  wl.push_back(L'n');
  wl.push_back(L'g');

  v.push_back('f');
  v.push_back('u');
  v.push_back('z');

  wv.push_back(L'w');
  wv.push_back(L'f');
  wv.push_back(L'u');
  wv.push_back(L'z');

  test_overloads();
  test_constructors();
  test_assignments();
  test_appends();
  test_modifiers();
  test_observers();
  test_relationals();
  test_inserter_and_extractor();
  test_other_non_members();
  test_iterators();
  test_decompositions();
  test_queries();
  test_imbue_locale();
  test_codecvt_argument();
  test_error_handling();

# if 0

  test_user_supplied_type();

#endif

  std::string foo("\\abc");
  const char* bar = "/abc";

  if (foo == bar)
    cout << "unintended consequence\n";

  return ::boost::report_errors();
}
