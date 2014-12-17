//  Example use of Microsoft TCHAR  ----------------------------------------------------//

//  Copyright Beman Dawes 2008

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <string>
#include <cassert>
#include <windows.h>
#include <winnt.h>

namespace fs = boost::filesystem;

typedef std::basic_string<TCHAR> tstring;

void func( const fs::path & p )
{
  assert( fs::exists( p ) );
}

int main()
{
  // get a path that is known to exist
  fs::path cp = fs::current_path(); 

  // demo: get tstring from the path
  tstring cp_as_tstring = cp.string<tstring>();

  // demo: pass tstring to filesystem function taking path
  assert( fs::exists( cp_as_tstring ) );

  // demo: pass tstring to user function taking path
  func( cp_as_tstring );

  return 0;
}
