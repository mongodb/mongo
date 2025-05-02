// Copyright (C) 2006 Douglas Gregor <doug.gregor -at- gmail.com>

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#include <boost/mpi/timer.hpp>
#include <boost/mpi/exception.hpp>

namespace boost { namespace mpi {

bool timer::time_is_global()
{
  int* is_global;
  int found = 0;

  BOOST_MPI_CHECK_RESULT(MPI_Comm_get_attr,
                         (MPI_COMM_WORLD, MPI_WTIME_IS_GLOBAL, &is_global,
                          &found));
  if (!found)
    return false;
  else
    return *is_global != 0;
}

} } // end namespace boost::mpi
