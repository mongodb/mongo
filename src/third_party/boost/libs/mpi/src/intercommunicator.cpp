// Copyright (C) 2007 Trustees of Indiana University

// Authors: Douglas Gregor
//          Andrew Lumsdaine

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#include <boost/mpi/intercommunicator.hpp>
#include <boost/mpi/environment.hpp>
#include <boost/mpi/group.hpp>

namespace boost { namespace mpi {

intercommunicator::intercommunicator(const communicator& local, 
                                     int local_leader,
                                     const communicator& peer, 
                                     int remote_leader)
{
  MPI_Comm comm;
  BOOST_MPI_CHECK_RESULT(MPI_Intercomm_create,
                         ((MPI_Comm)local, local_leader,
                          (MPI_Comm)peer, remote_leader,
                          environment::collectives_tag(), &comm));
  comm_ptr.reset(new MPI_Comm(comm), comm_free());
}

boost::mpi::group intercommunicator::local_group() const
{
  return this->group();
}

int intercommunicator::remote_size() const
{
  int size;
  BOOST_MPI_CHECK_RESULT(MPI_Comm_remote_size, ((MPI_Comm)*this, &size));
  return size;
}

boost::mpi::group intercommunicator::remote_group() const
{
  MPI_Group gr;
  BOOST_MPI_CHECK_RESULT(MPI_Comm_remote_group, ((MPI_Comm)*this, &gr));
  return boost::mpi::group(gr, /*adopt=*/true);
}

communicator intercommunicator::merge(bool high) const
{
  MPI_Comm comm;
  BOOST_MPI_CHECK_RESULT(MPI_Intercomm_merge, ((MPI_Comm)*this, high, &comm));
  return communicator(comm, comm_take_ownership);
}

} } // end namespace boost::mpi
