// Copyright (C) 2007 Trustees of Indiana University

// Authors: Douglas Gregor
//          Andrew Lumsdaine

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#include <boost/mpi/exception.hpp>
#include <boost/mpi/error_string.hpp>

namespace boost { namespace mpi {

exception::exception(const char* routine, int result_code)
  : routine_(routine), result_code_(result_code)
{
  message.append(routine_);
  message.append(": ");
  message.append(error_string(result_code));
}

exception::~exception() throw() { }

} } // end namespace boost::mpi
