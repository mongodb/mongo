// Copyright 2010, Niels Dekker.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// Test program for the boost::value_initialized<T> workaround.
//
// 17 June 2010 (Created) Niels Dekker

// Switch the workaround off, before inluding "value_init.hpp".
#define BOOST_DETAIL_VALUE_INIT_WORKAROUND 0
#include <boost/utility/value_init.hpp>

#include <iostream>  // For cout.
#include <cstdlib>  // For EXIT_SUCCESS and EXIT_FAILURE.

namespace
{
  struct empty_struct
  {
  };

  // A POD aggregate struct derived from an empty struct.
  // Similar to struct Foo1 from Microsoft Visual C++ bug report 484295,
  // "VC++ does not value-initialize members of derived classes without 
  // user-declared constructor", reported in 2009 by Sylvester Hesp:
  // https://connect.microsoft.com/VisualStudio/feedback/details/484295
  struct derived_struct: empty_struct
  {
    int data;
  };

  bool is_value_initialized(const derived_struct& arg)
  {
    return arg.data == 0;
  }


  class virtual_destructor_holder
  {
  public:
    int i;
    virtual ~virtual_destructor_holder()
    {
    }
  };

  bool is_value_initialized(const virtual_destructor_holder& arg)
  {
    return arg.i == 0;
  }

  // Equivalent to the Stats class from GCC Bug 33916,
  // "Default constructor fails to initialize array members", reported in 2007 by
  // Michael Elizabeth Chastain: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=33916
  // and fixed for GCC 4.2.4.
  class private_int_array_pair
  {
    friend bool is_value_initialized(const private_int_array_pair& arg);
  private:
    int first[12];
    int second[12];
  };

  bool is_value_initialized(const private_int_array_pair& arg)
  {
    for ( unsigned i = 0; i < 12; ++i)
    {
      if ( (arg.first[i] != 0) || (arg.second[i] != 0) )
      {
        return false;
      }
    }
    return true;
  }

  template <typename T>
  bool is_value_initialized(const T(& arg)[2])
  {
    return
      is_value_initialized(arg[0]) &&
      is_value_initialized(arg[1]);
  }

  template <typename T>
  bool is_value_initialized(const boost::value_initialized<T>& arg)
  {
    return is_value_initialized(arg.data());
  }

  // Returns zero when the specified object is value-initializated, and one otherwise.
  // Prints a message to standard output if the value-initialization has failed.
  template <class T>
  unsigned failed_to_value_initialized(const T& object, const char *const object_name)
  {
    if ( is_value_initialized(object) )
    {
      return 0u;
    }
    else
    {
      std::cout << "Note: Failed to value-initialize " << object_name << '.' << std::endl;
      return 1u;
    }
  }

// A macro that passed both the name and the value of the specified object to
// the function above here.
#define FAILED_TO_VALUE_INITIALIZE(value) failed_to_value_initialized(value, #value)

  // Equivalent to the dirty_stack() function from GCC Bug 33916,
  // "Default constructor fails to initialize array members", reported in 2007 by
  // Michael Elizabeth Chastain: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=33916
  void dirty_stack()
  {
    unsigned char array_on_stack[4096];
    for (unsigned i = 0; i < sizeof(array_on_stack); ++i)
    {
      array_on_stack[i] = 0x11;
    }
  }

}


int main()
{
  dirty_stack();

  // TODO More types may be added later.
  const unsigned num_failures =
    FAILED_TO_VALUE_INITIALIZE(boost::value_initialized<derived_struct>()) +
    FAILED_TO_VALUE_INITIALIZE(boost::value_initialized<virtual_destructor_holder[2]>()) +
	FAILED_TO_VALUE_INITIALIZE(boost::value_initialized<private_int_array_pair>());

#ifdef BOOST_DETAIL_VALUE_INIT_WORKAROUND_SUGGESTED
  // One or more failures are expected.
  return num_failures > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#else
  // No failures are expected.
  return num_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}
