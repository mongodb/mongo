
//          Copyright Alain Miniussi 2014.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

// Authors: Alain Miniussi

#include <algorithm>
#include <cassert>

#include <boost/mpi/cartesian_communicator.hpp>
#include <boost/mpi/detail/antiques.hpp>

namespace boost { namespace mpi {

std::ostream&
operator<<(std::ostream& out, cartesian_dimension const& d) {
  out << '(' << d.size << ',';
  if (d.periodic) {
    out << "periodic";
  } else {
    out << "bounded";
  }
  out << ')';
  return out;
}

std::ostream&
operator<<(std::ostream& out, cartesian_topology const& topo) {
  out << '{';
  int const sz = topo.size();
  for (int i = 0; i < sz; ++i) {
    out << topo[i];
    if ( i < (sz-1) ) {
      out << ',';
    }
  }
  out << '}';
  return out;
}

cartesian_communicator::cartesian_communicator(const communicator&         comm,
                                               const cartesian_topology&   topology,
                                               bool                        reorder )
  : communicator(MPI_COMM_NULL, comm_attach) 
{
  std::vector<int> dims(topology.size());
  std::vector<int> periodic(topology.size());
  int tsz = topology.size();
  for(int i = 0; i < tsz; ++i) {
    dims[i]     = topology[i].size;
    periodic[i] = topology[i].periodic;
  }
  // Fill the gaps, if any
  if (std::count(dims.begin(), dims.end(), 0) > 0) {
    cartesian_dimensions(comm, dims);
  }
  MPI_Comm newcomm;
  BOOST_MPI_CHECK_RESULT(MPI_Cart_create, 
                         ((MPI_Comm)comm, dims.size(),
                          detail::c_data(dims), detail::c_data(periodic),
                          int(reorder), &newcomm));
  if(newcomm != MPI_COMM_NULL) {
    comm_ptr.reset(new MPI_Comm(newcomm), comm_free());
  }
}

cartesian_communicator::cartesian_communicator(const cartesian_communicator& comm,
                                               const std::vector<int>&       keep ) 
  : communicator(MPI_COMM_NULL, comm_attach) 
{
  int const max_dims = comm.ndims();
  int const nbkept = keep.size();
  assert(nbkept <= max_dims);
  std::vector<int> bitset(max_dims, int(false));
  for(int i = 0; i < nbkept; ++i) {
    assert(keep[i] < max_dims);
    bitset[keep[i]] = true;
  }
  
  MPI_Comm newcomm;
  BOOST_MPI_CHECK_RESULT(MPI_Cart_sub, 
                         ((MPI_Comm)comm, detail::c_data(bitset), &newcomm));
  if(newcomm != MPI_COMM_NULL) {
    comm_ptr.reset(new MPI_Comm(newcomm), comm_free());
  }
}

int
cartesian_communicator::ndims() const {
  int n = -1;
  BOOST_MPI_CHECK_RESULT(MPI_Cartdim_get, 
                         (MPI_Comm(*this), &n));
  return n;
}

int
cartesian_communicator::rank(const std::vector<int>& coords ) const {
  int r = -1;
  assert(int(coords.size()) == ndims());
  BOOST_MPI_CHECK_RESULT(MPI_Cart_rank, 
                         (MPI_Comm(*this), detail::c_data(const_cast<std::vector<int>&>(coords)), 
                          &r));
  return r;
}

std::pair<int, int>
cartesian_communicator::shifted_ranks(int dim, int disp) const {
  std::pair<int, int> r(-1,-1);
  assert(0 <= dim && dim < ndims());
  BOOST_MPI_CHECK_RESULT(MPI_Cart_shift, 
                         (MPI_Comm(*this), dim, disp, &(r.first), &(r.second)));
  return r;
}

std::vector<int>
cartesian_communicator::coordinates(int rk) const {
  std::vector<int> cbuf(ndims());
  BOOST_MPI_CHECK_RESULT(MPI_Cart_coords, 
                         (MPI_Comm(*this), rk, cbuf.size(), detail::c_data(cbuf) ));
  return cbuf;
}

void
cartesian_communicator::topology(  cartesian_topology&  topo,
                                   std::vector<int>&  coords ) const {
  int ndims = this->ndims();
  topo.resize(ndims);
  coords.resize(ndims);
  std::vector<int> cdims(ndims);
  std::vector<int> cperiods(ndims);
  BOOST_MPI_CHECK_RESULT(MPI_Cart_get,
                         (MPI_Comm(*this), ndims, detail::c_data(cdims), detail::c_data(cperiods), detail::c_data(coords)));
  cartesian_topology res(cdims.begin(), cperiods.begin(), ndims);
  topo.swap(res);
}

cartesian_topology
cartesian_communicator::topology() const {
  cartesian_topology topo(ndims());
  std::vector<int> coords;
  topology(topo, coords);
  return topo;
}

void
cartesian_topology::split(std::vector<int>& dims, std::vector<bool>& periodics) const {
  int ndims = size();
  dims.resize(ndims);
  periodics.resize(ndims);
  for(int i = 0; i < ndims; ++i) {
    cartesian_dimension const& d = (*this)[i];
    dims[i]      = d.size;
    periodics[i] = d.periodic;
  }
}

std::vector<int>&
cartesian_dimensions(int sz, std::vector<int>&  dims) {
  int min = 1;
  int const dimsz = dims.size();
  for(int i = 0; i < dimsz; ++i) {
    if (dims[i] > 0) {
      min *= dims[i];
    }
  }
  int leftover = sz % min;
  
  BOOST_MPI_CHECK_RESULT(MPI_Dims_create,
                         (sz-leftover, dims.size(), detail::c_data(dims)));
  return dims;
}

} } // end namespace boost::mpi
