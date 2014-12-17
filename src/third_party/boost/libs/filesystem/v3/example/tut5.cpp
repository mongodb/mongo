//  filesystem tut5.cpp  ---------------------------------------------------------------//

//  Copyright Beman Dawes 2010

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#include <boost/filesystem/fstream.hpp>
#include <string>
#include <list>
namespace fs = boost::filesystem;

int main()
{
  // \u263A is "Unicode WHITE SMILING FACE = have a nice day!"
  std::string narrow_string ("smile2");
  std::wstring wide_string (L"smile2\u263A");
  std::list<char> narrow_list;
  narrow_list.push_back('s');
  narrow_list.push_back('m');
  narrow_list.push_back('i');
  narrow_list.push_back('l');
  narrow_list.push_back('e');
  narrow_list.push_back('3');
  std::list<wchar_t> wide_list;
  wide_list.push_back(L's');
  wide_list.push_back(L'm');
  wide_list.push_back(L'i');
  wide_list.push_back(L'l');
  wide_list.push_back(L'e');
  wide_list.push_back(L'3');
  wide_list.push_back(L'\u263A');

  { fs::ofstream f("smile"); }
  { fs::ofstream f(L"smile\u263A"); }
  { fs::ofstream f(narrow_string); }
  { fs::ofstream f(wide_string); }
  { fs::ofstream f(narrow_list); }
  { fs::ofstream f(wide_list); }
  narrow_list.pop_back();
  narrow_list.push_back('4');
  wide_list.pop_back();
  wide_list.pop_back();
  wide_list.push_back(L'4');
  wide_list.push_back(L'\u263A');
  { fs::ofstream f(fs::path(narrow_list.begin(), narrow_list.end())); }
  { fs::ofstream f(fs::path(wide_list.begin(), wide_list.end())); }

  return 0;
}
