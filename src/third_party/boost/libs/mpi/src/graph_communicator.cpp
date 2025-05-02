// Copyright (C) 2007 Trustees of Indiana University

// Authors: Douglas Gregor
//          Andrew Lumsdaine

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#include <boost/mpi/graph_communicator.hpp>

namespace boost { namespace mpi {

// Incidence Graph requirements
std::pair<detail::comm_out_edge_iterator, detail::comm_out_edge_iterator>
out_edges(int vertex, const graph_communicator& comm)
{
  int nneighbors = out_degree(vertex, comm);
  shared_array<int> neighbors(new int[nneighbors]);
  BOOST_MPI_CHECK_RESULT(MPI_Graph_neighbors, 
                         ((MPI_Comm)comm, vertex, nneighbors, neighbors.get()));
  return std::make_pair(detail::comm_out_edge_iterator(vertex, neighbors, 0),
                        detail::comm_out_edge_iterator(vertex, neighbors, 
                                                       nneighbors));
}

int out_degree(int vertex, const graph_communicator& comm)
{
  int nneighbors;
  BOOST_MPI_CHECK_RESULT(MPI_Graph_neighbors_count, 
                         ((MPI_Comm)comm, vertex, &nneighbors));
  return nneighbors;
}

// Adjacency Graph requirements
std::pair<detail::comm_adj_iterator, detail::comm_adj_iterator>
adjacent_vertices(int vertex, const graph_communicator& comm)
{
  int nneighbors = out_degree(vertex, comm);
  shared_array<int> neighbors(new int[nneighbors]);
  BOOST_MPI_CHECK_RESULT(MPI_Graph_neighbors, 
                         ((MPI_Comm)comm, vertex, nneighbors, neighbors.get()));
  return std::make_pair(detail::comm_adj_iterator(neighbors, 0),
                        detail::comm_adj_iterator(neighbors, nneighbors));
}

// Edge List Graph requirements
std::pair<detail::comm_edge_iterator, detail::comm_edge_iterator>
edges(const graph_communicator& comm);

std::pair<detail::comm_edge_iterator, detail::comm_edge_iterator>
edges(const graph_communicator& comm)
{
  int nnodes, nedges;
  BOOST_MPI_CHECK_RESULT(MPI_Graphdims_get, ((MPI_Comm)comm, &nnodes, &nedges));

  shared_array<int> indices(new int[nnodes]);
  shared_array<int> edges(new int[nedges]);
  BOOST_MPI_CHECK_RESULT(MPI_Graph_get,
                         ((MPI_Comm)comm, nnodes, nedges, 
                          indices.get(), edges.get()));
  return std::make_pair(detail::comm_edge_iterator(indices, edges),
                        detail::comm_edge_iterator(nedges));
}


int num_edges(const graph_communicator& comm)
{
  int nnodes, nedges;
  BOOST_MPI_CHECK_RESULT(MPI_Graphdims_get, ((MPI_Comm)comm, &nnodes, &nedges));
  return nedges;
}

} } // end namespace boost::mpi
