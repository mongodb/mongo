// Copyright 2005 Douglas Gregor.

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// Message Passing Interface 1.1 -- Section 3. MPI Point-to-point

/* There is the potential for optimization here. We could keep around
   a "small message" buffer of size N that we just receive into by
   default. If the message is N - sizeof(int) bytes or smaller, it can
   just be sent with that buffer. If it's larger, we send the first N
   - sizeof(int) bytes in the first packet followed by another
   packet. The size of the second packet will be stored in an integer
   at the end of the first packet.

   We will introduce this optimization later, when we have more
   performance test cases and have met our functionality goals. */

#include <boost/mpi/detail/point_to_point.hpp>
#include <boost/mpi/datatype.hpp>
#include <boost/mpi/exception.hpp>
#include <boost/mpi/request.hpp>
#include <boost/mpi/communicator.hpp>
#include <boost/mpi/detail/antiques.hpp>
#include <cassert>

namespace boost { namespace mpi { namespace detail {

void
packed_archive_send(communicator const& comm, int dest, int tag,
                    const packed_oarchive& ar)
{
#if defined(BOOST_MPI_USE_IMPROBE)
  {
    void *buf = detail::unconst(ar.address());
    BOOST_MPI_CHECK_RESULT(MPI_Send,
                           (buf, ar.size(), MPI_PACKED,
                            dest, tag, comm));
  }
#else
  {
    std::size_t const& size = ar.size();
    BOOST_MPI_CHECK_RESULT(MPI_Send,
                           (detail::unconst(&size), 1, 
                            get_mpi_datatype(size), 
                            dest, tag, comm));
    BOOST_MPI_CHECK_RESULT(MPI_Send,
                           (detail::unconst(ar.address()), size,
                            MPI_PACKED,
                            dest, tag, comm));
  }
#endif
}

request
packed_archive_isend(communicator const& comm, int dest, int tag,
                     const packed_oarchive& ar)
{
  return request::make_packed_send(comm, dest, tag, 
                                   detail::unconst(ar.address()), ar.size());
}

request
packed_archive_isend(communicator const& comm, int dest, int tag,
                     const packed_iarchive& ar)
{
  return request::make_packed_send(comm, dest, tag, 
                                   detail::unconst(ar.address()), ar.size());
}

void
packed_archive_recv(communicator const& comm, int source, int tag, packed_iarchive& ar,
                    MPI_Status& status)
{
#if defined(BOOST_MPI_USE_IMPROBE)
  {
    MPI_Message msg;
    BOOST_MPI_CHECK_RESULT(MPI_Mprobe, (source, tag, comm, &msg, &status));
    int count;
    BOOST_MPI_CHECK_RESULT(MPI_Get_count, (&status, MPI_PACKED, &count));
    ar.resize(count);
    BOOST_MPI_CHECK_RESULT(MPI_Mrecv, (ar.address(), count, MPI_PACKED, &msg, &status));
  } 
#else
  {
    std::size_t count;
    BOOST_MPI_CHECK_RESULT(MPI_Recv,
                           (&count, 1, get_mpi_datatype(count),
                            source, tag, comm, &status));
    
    // Prepare input buffer and receive the message
    ar.resize(count);
    BOOST_MPI_CHECK_RESULT(MPI_Recv,
                           (ar.address(), count, MPI_PACKED,
                            status.MPI_SOURCE, status.MPI_TAG,
                            comm, &status));
  }
#endif
}

} } } // end namespace boost::mpi::detail
