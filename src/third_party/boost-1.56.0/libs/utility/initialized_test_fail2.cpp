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
  void from_value_initialized_to_initialized()
  {
    boost::value_initialized<int> value_initialized_int;

    // Okay: initialized<T> can be initialized by value_initialized<T>.
    boost::initialized<int> initialized_int(value_initialized_int);
  }

  void from_initialized_to_value_initialized()
  {
    boost::initialized<int> initialized_int(13);

    // The following line should not compile, because initialized<T>
    // should not be convertible to value_initialized<T>.
    boost::value_initialized<int> value_initialized_int(initialized_int);
  }
}

int main()
{
  // This should fail to compile, so there is no need to call any function.
  return 0;
}
