//  locale_info.cpp  ---------------------------------------------------------//

//  Copyright Beman Dawes 2011

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

#include <locale>
#include <iostream>
#include <exception>
#include <cstdlib>
using namespace std;

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4996)  // ... Function call with parameters that may be unsafe
#endif

namespace
{
  void facet_info(const locale& loc, const char* msg)
  {
    cout << "has_facet<std::codecvt<wchar_t, char, std::mbstate_t> >("
      << msg << ") is "
      << (has_facet<std::codecvt<wchar_t, char, std::mbstate_t> >(loc)
          ? "true\n"
          : "false\n");
  }

  void default_info()
  {
    try
    {
      locale loc;
      cout << "\nlocale default construction OK" << endl;
      facet_info(loc, "locale()");
    }
    catch (const exception& ex)
    {
      cout << "\nlocale default construction threw: " << ex.what() << endl;
    }
  }

  void null_string_info()
  {
    try
    {
      locale loc("");
      cout << "\nlocale(\"\") construction OK" << endl;
      facet_info(loc, "locale(\"\")");
    }
    catch (const exception& ex)
    {
      cout << "\nlocale(\"\") construction threw: " << ex.what() << endl;
    }
  }

  void classic_info()
  {
    try
    {
      locale loc(locale::classic());
      cout << "\nlocale(locale::classic()) copy construction OK" << endl;
      facet_info(loc, "locale::classic()");
    }
    catch (const exception& ex)
    {
      cout << "\nlocale(locale::clasic()) copy construction threw: " << ex.what() << endl;
    }
  }
}

int main()
{
  const char* lang = getenv("LANG");
  cout << "\nLANG environmental variable is "
    << (lang ? lang : "not present") << endl;

  default_info();
  null_string_info();
  classic_info();

  return 0;
}

#ifdef _MSC_VER
#  pragma warning(pop)
#endif
