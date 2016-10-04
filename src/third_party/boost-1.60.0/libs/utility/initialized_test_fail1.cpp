// Copyright 2010, Niels Dekker.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// Test program for boost::initialized<T>. Must fail to compile.
//
// Initial: 2 May 2010

#include <boost/utility/value_init.hpp>

namespace
{
  void direct_initialize_from_int()
  {
    // Okay: initialized<T> supports direct-initialization from T.
    boost::initialized<int> direct_initialized_int(1);
  }

  void copy_initialize_from_int()
  {
    // The following line should not compile, because initialized<T> 
    // was not intended to supports copy-initialization from T.
    boost::initialized<int> copy_initialized_int = 1;
  }
}

int main()
{
  // This should fail to compile, so there is no need to call any function.
  return 0;
}
