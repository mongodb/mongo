// Copyright (C) 2005, 2006 Douglas Gregor.

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#include <boost/mpi/communicator.hpp>
#include <boost/mpi/group.hpp>
#include <boost/mpi/intercommunicator.hpp>
#include <boost/mpi/graph_communicator.hpp>
#include <boost/mpi/cartesian_communicator.hpp>
#include <boost/mpi/skeleton_and_content.hpp>
#include <boost/mpi/detail/point_to_point.hpp>

namespace boost { namespace mpi {

communicator::communicator()
{
  comm_ptr.reset(new MPI_Comm(MPI_COMM_WORLD));
}

communicator::communicator(const MPI_Comm& comm, comm_create_kind kind)
{
  if (comm == MPI_COMM_NULL)
    /* MPI_COMM_NULL indicates that the communicator is not usable. */
    return;

  switch (kind) {
  case comm_duplicate:
    {
      MPI_Comm newcomm;
      BOOST_MPI_CHECK_RESULT(MPI_Comm_dup, (comm, &newcomm));
      comm_ptr.reset(new MPI_Comm(newcomm), comm_free());
      MPI_Comm_set_errhandler(newcomm, MPI_ERRORS_RETURN);
      break;
    }

  case comm_take_ownership:
    comm_ptr.reset(new MPI_Comm(comm), comm_free());
    break;

  case comm_attach:
    comm_ptr.reset(new MPI_Comm(comm));
    break;
  }
}

communicator::communicator(const communicator& comm, 
                           const boost::mpi::group& subgroup)
{
  MPI_Comm newcomm;
  BOOST_MPI_CHECK_RESULT(MPI_Comm_create, 
                         ((MPI_Comm)comm, (MPI_Group)subgroup, &newcomm));
  if(newcomm != MPI_COMM_NULL)
    comm_ptr.reset(new MPI_Comm(newcomm), comm_free());
}

int communicator::size() const
{
  int size_;
  BOOST_MPI_CHECK_RESULT(MPI_Comm_size, (MPI_Comm(*this), &size_));
  return size_;
}

int communicator::rank() const
{
  int rank_;
  BOOST_MPI_CHECK_RESULT(MPI_Comm_rank, (MPI_Comm(*this), &rank_));
  return rank_;
}

boost::mpi::group communicator::group() const
{
  MPI_Group gr;
  BOOST_MPI_CHECK_RESULT(MPI_Comm_group, ((MPI_Comm)*this, &gr));
  return boost::mpi::group(gr, /*adopt=*/true);
}

void communicator::send(int dest, int tag) const
{
  BOOST_MPI_CHECK_RESULT(MPI_Send,
                         (MPI_BOTTOM, 0, MPI_PACKED,
                          dest, tag, MPI_Comm(*this)));
}

status communicator::recv(int source, int tag) const
{
  status stat;
  BOOST_MPI_CHECK_RESULT(MPI_Recv,
                         (MPI_BOTTOM, 0, MPI_PACKED,
                          source, tag, MPI_Comm(*this), &stat.m_status));
  return stat;
}

optional<status> communicator::iprobe(int source, int tag) const
{
  typedef optional<status> result_type;

  status stat;
  int flag;
  BOOST_MPI_CHECK_RESULT(MPI_Iprobe,
                         (source, tag, MPI_Comm(*this), &flag,
                          &stat.m_status));
  if (flag) return stat;
  else return result_type();
}

status communicator::probe(int source, int tag) const
{
  status stat;
  BOOST_MPI_CHECK_RESULT(MPI_Probe,
                         (source, tag, MPI_Comm(*this), &stat.m_status));
  return stat;
}

void (communicator::barrier)() const
{
  BOOST_MPI_CHECK_RESULT(MPI_Barrier, (MPI_Comm(*this)));
}


communicator::operator MPI_Comm() const
{
  if (comm_ptr) return *comm_ptr;
  else return MPI_COMM_NULL;
}

communicator communicator::split(int color) const
{
  return split(color, rank());
}

communicator communicator::split(int color, int key) const
{
  MPI_Comm newcomm;
  BOOST_MPI_CHECK_RESULT(MPI_Comm_split,
                         (MPI_Comm(*this), color, key, &newcomm));
  return communicator(newcomm, comm_take_ownership);
}

optional<intercommunicator> communicator::as_intercommunicator() const
{
  int flag;
  BOOST_MPI_CHECK_RESULT(MPI_Comm_test_inter, ((MPI_Comm)*this, &flag));
  if (flag)
    return intercommunicator(comm_ptr);
  else
    return optional<intercommunicator>();
}

bool communicator::has_graph_topology() const
{
  bool is_graph = false;
  // topology test not allowed on MPI_NULL_COMM
  if (bool(*this)) {
    int status;
    BOOST_MPI_CHECK_RESULT(MPI_Topo_test, ((MPI_Comm)*this, &status));
    is_graph = status == MPI_GRAPH;
  }
  return is_graph;
}

optional<graph_communicator> communicator::as_graph_communicator() const
{
  if (has_graph_topology()) {
    return graph_communicator(comm_ptr);
  } else {
    return optional<graph_communicator>();
  }
}

bool communicator::has_cartesian_topology() const
{
  bool is_cart = false;
  // topology test not allowed on MPI_NULL_COM
  if (bool(*this)) {
    int status;
    BOOST_MPI_CHECK_RESULT(MPI_Topo_test, ((MPI_Comm)*this, &status));
    is_cart = status == MPI_CART;
  }
  return is_cart;
}

optional<cartesian_communicator> communicator::as_cartesian_communicator() const
{
  if (has_cartesian_topology()) {
    return cartesian_communicator(comm_ptr);
  } else {
    return optional<cartesian_communicator>();
  }
}

void communicator::abort(int errcode) const
{
  BOOST_MPI_CHECK_RESULT(MPI_Abort, (MPI_Comm(*this), errcode));
  std::abort();
}

/*************************************************************
 * archived send/recv                                     *
 *************************************************************/
template<>
void
communicator::send<packed_oarchive>(int dest, int tag,
                                    const packed_oarchive& ar) const
{
  detail::packed_archive_send(*this, dest, tag, ar);
}

template<>
void
communicator::send<packed_skeleton_oarchive>
  (int dest, int tag, const packed_skeleton_oarchive& ar) const
{
  this->send(dest, tag, ar.get_skeleton());
}

template<>
void communicator::send<content>(int dest, int tag, const content& c) const
{
  BOOST_MPI_CHECK_RESULT(MPI_Send,
                         (MPI_BOTTOM, 1, c.get_mpi_datatype(),
                          dest, tag, MPI_Comm(*this)));
}

template<>
status
communicator::recv<packed_iarchive>(int source, int tag,
                                    packed_iarchive& ar) const
{
  status stat;
  detail::packed_archive_recv(*this, source, tag, ar,
                              stat.m_status);
  return stat;
}

template<>
status
communicator::recv<packed_skeleton_iarchive>
  (int source, int tag, packed_skeleton_iarchive& ar) const
{
  return this->recv(source, tag, ar.get_skeleton());
}

template<>
status
communicator::recv<const content>(int source, int tag, const content& c) const
{
  status stat;
  BOOST_MPI_CHECK_RESULT(MPI_Recv,
                         (MPI_BOTTOM, 1, c.get_mpi_datatype(),
                          source, tag, MPI_Comm(*this), &stat.m_status));
  return stat;
}

/*************************************************************
 * non-blocking send/recv                                    *
 *************************************************************/
template<>
request
communicator::isend<packed_oarchive>(int dest, int tag,
                                     const packed_oarchive& ar) const
{
  return detail::packed_archive_isend(*this, dest, tag, ar);
}

template<>
request
communicator::isend<packed_skeleton_oarchive>
  (int dest, int tag, const packed_skeleton_oarchive& ar) const
{
  return this->isend(dest, tag, ar.get_skeleton());
}

template<>
request communicator::isend<content>(int dest, int tag, const content& c) const
{
  return request::make_bottom_send(*this, dest, tag, c.get_mpi_datatype());
}

request communicator::isend(int dest, int tag) const
{
  return request::make_empty_send(*this, dest, tag);
}

template<>
request
communicator::irecv<packed_skeleton_iarchive>
  (int source, int tag, packed_skeleton_iarchive& ar) const
{
  return this->irecv(source, tag, ar.get_skeleton());
}

template<>
request
communicator::irecv<const content>(int source, int tag,
                                   const content& c) const
{
  return request::make_bottom_recv(*this, source, tag, c.get_mpi_datatype());
}

request communicator::irecv(int source, int tag) const
{
  return request::make_empty_recv(*this, source, tag);
}

bool operator==(const communicator& comm1, const communicator& comm2)
{
  int result;
  BOOST_MPI_CHECK_RESULT(MPI_Comm_compare,
                         (MPI_Comm(comm1), MPI_Comm(comm2), &result));
  return result == MPI_IDENT;
}

} } // end namespace boost::mpi
