// Copyright 2005 Douglas Gregor.

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// Message Passing Interface 1.1 -- Section 4.4. Broadcast

#include <boost/mpi/config.hpp>
#include <boost/mpi/collectives/broadcast.hpp>
#include <boost/mpi/skeleton_and_content.hpp>
#include <boost/mpi/detail/point_to_point.hpp>
#include <boost/mpi/environment.hpp>
#include <cassert>

namespace boost { namespace mpi {

template<>
void
broadcast<const packed_oarchive>(const communicator& comm,
                                 const packed_oarchive& oa,
                                 int root)
{
  // Only the root can broadcast the packed_oarchive
  assert(comm.rank() == root);

  int size = comm.size();
  if (size < 2) return;

  // Determine maximum tag value
  int tag = environment::collectives_tag();

  // Broadcast data to all nodes
  std::vector<request> requests(size-1);
  std::vector<request>::iterator it = requests.begin();
  for (int dest = 0; dest < size; ++dest) {
    if (dest != root) {
      *it++ = detail::packed_archive_isend(comm, dest, tag, oa);
    }
  }
  wait_all(requests.begin(), requests.end());
}

template<>
void
broadcast<packed_oarchive>(const communicator& comm, packed_oarchive& oa,
                           int root)
{
  broadcast(comm, const_cast<const packed_oarchive&>(oa), root);
}

template<>
void
broadcast<packed_iarchive>(const communicator& comm, packed_iarchive& ia,
                           int root)
{
  int size = comm.size();
  if (size < 2) return;

  // Determine maximum tag value
  int tag = environment::collectives_tag();

  // Receive data from the root.
  if (comm.rank() != root) {
    MPI_Status status;
    detail::packed_archive_recv(comm, root, tag, ia, status);
  } else {
    // Broadcast data to all nodes
    std::vector<request> requests(size-1);
    std::vector<request>::iterator it = requests.begin();
    for (int dest = 0; dest < size; ++dest) {
      if (dest != root) {
        *it++ = detail::packed_archive_isend(comm, dest, tag, ia);
      }
    }
    wait_all(requests.begin(), requests.end());
  }
}

template<>
void
broadcast<const packed_skeleton_oarchive>(const communicator& comm,
                                          const packed_skeleton_oarchive& oa,
                                          int root)
{
  broadcast(comm, oa.get_skeleton(), root);
}

template<>
void
broadcast<packed_skeleton_oarchive>(const communicator& comm,
                                    packed_skeleton_oarchive& oa, int root)
{
  broadcast(comm, oa.get_skeleton(), root);
}

template<>
void
broadcast<packed_skeleton_iarchive>(const communicator& comm,
                                    packed_skeleton_iarchive& ia, int root)
{
  broadcast(comm, ia.get_skeleton(), root);
}

template<>
void broadcast<content>(const communicator& comm, content& c, int root)
{
  broadcast(comm, const_cast<const content&>(c), root);
}

template<>
void broadcast<const content>(const communicator& comm, const content& c,
                              int root)
{
#if defined(BOOST_MPI_BCAST_BOTTOM_WORKS_FINE)
  BOOST_MPI_CHECK_RESULT(MPI_Bcast,
                         (MPI_BOTTOM, 1, c.get_mpi_datatype(),
                          root, comm));
#else
  if (comm.size() < 2)
    return;

  // Some versions of LAM/MPI behave badly when broadcasting using
  // MPI_BOTTOM, so we'll instead use manual send/recv operations.
  if (comm.rank() == root) {
    for (int p = 0; p < comm.size(); ++p) {
      if (p != root) {
        BOOST_MPI_CHECK_RESULT(MPI_Send,
                               (MPI_BOTTOM, 1, c.get_mpi_datatype(),
                                p, environment::collectives_tag(), comm));
      }
    }
  } else {
    BOOST_MPI_CHECK_RESULT(MPI_Recv,
                           (MPI_BOTTOM, 1, c.get_mpi_datatype(),
                            root, environment::collectives_tag(),
                            comm, MPI_STATUS_IGNORE));
  }
#endif
}

} } // end namespace boost::mpi
