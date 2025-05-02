// Copyright (C) 2005, 2006 Douglas Gregor.

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include <boost/mpi/status.hpp>

namespace boost { namespace mpi {

bool status::cancelled() const
{
  int flag = 0;
  BOOST_MPI_CHECK_RESULT(MPI_Test_cancelled, (&m_status, &flag));
  return flag != 0;
}

}}
