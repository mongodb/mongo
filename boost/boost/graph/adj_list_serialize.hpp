//=======================================================================
// Copyright 2005 Jeremy G. Siek
// Authors: Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================
#ifndef ADJ_LIST_SERIALIZE_HPP
#define ADJ_LIST_SERIALIZE_HPP

#include <boost/graph/adjacency_list.hpp>
#include <boost/pending/property_serialize.hpp>
#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#include <boost/serialization/collections_save_imp.hpp>
#include <boost/serialization/collections_load_imp.hpp>
#include <boost/serialization/split_free.hpp>

namespace boost { 

namespace serialization {

// Turn off tracking for adjacency_list. It's not polymorphic, and we
// need to do this to enable saving of non-const adjacency lists.
template<class OEL, class VL, class D, class VP, class EP, class GP, class EL>
struct tracking_level<boost::adjacency_list<OEL,VL,D,VP,EP,GP,EL> > {
  typedef mpl::integral_c_tag tag;
  typedef mpl::int_<track_never> type;
  BOOST_STATIC_CONSTANT(int, value = tracking_level::type::value);
};

template<class Archive, class OEL, class VL, class D, 
     class VP, class EP, class GP, class EL>
inline void save(
    Archive & ar,
    const boost::adjacency_list<OEL,VL,D,VP,EP,GP,EL> &graph,
    const unsigned int /* file_version */
){
  typedef adjacency_list<OEL,VL,D,VP,EP,GP,EL> Graph;
  typedef typename graph_traits<Graph>::vertex_descriptor Vertex;

  int V = num_vertices(graph);
  int E = num_edges(graph);
  ar << BOOST_SERIALIZATION_NVP(V);
  ar << BOOST_SERIALIZATION_NVP(E);

  // assign indices to vertices
  std::map<Vertex,int> indices;
  int num = 0;
  typename graph_traits<Graph>::vertex_iterator vi;
  for (vi = vertices(graph).first; vi != vertices(graph).second; ++vi) {
    indices[*vi] = num++;
    ar << serialization::make_nvp("vertex_property", get(vertex_all_t(), graph, *vi) );
  }
  
  // write edges
  typename graph_traits<Graph>::edge_iterator ei;
  for (ei = edges(graph).first; ei != edges(graph).second; ++ei){
    ar << serialization::make_nvp("u" , indices[source(*ei,graph)]);
    ar << serialization::make_nvp("v" , indices[target(*ei,graph)]);
    ar << serialization::make_nvp("edge_property", get(edge_all_t(), graph, *ei) );
  }
}


template<class Archive, class OEL, class VL, class D,
     class VP, class EP, class GP, class EL>
inline void load(
    Archive & ar,
    boost::adjacency_list<OEL,VL,D,VP,EP,GP,EL> &graph,
    const unsigned int /* file_version */
){
  typedef adjacency_list<OEL,VL,D,VP,EP,GP,EL> Graph;
  typedef typename graph_traits<Graph>::vertex_descriptor Vertex;
  typedef typename graph_traits<Graph>::edge_descriptor Edge;

  unsigned int V;
  ar >> BOOST_SERIALIZATION_NVP(V);
  unsigned int E;
  ar >> BOOST_SERIALIZATION_NVP(E);
  
  std::vector<Vertex> verts(V);
  int i = 0;
  while(V-- > 0){
    Vertex v = add_vertex(graph);
    verts[i++] = v;
    ar >> serialization::make_nvp("vertex_property", get(vertex_all_t(), graph, v) );
  }
  while(E-- > 0){
    int u; int v;
    ar >> BOOST_SERIALIZATION_NVP(u);
    ar >> BOOST_SERIALIZATION_NVP(v);
    Edge e; bool inserted;
    tie(e,inserted) = add_edge(verts[u], verts[v], graph);
    ar >> serialization::make_nvp("edge_property", get(edge_all_t(), graph, e) );
  }
}

template<class Archive, class OEL, class VL, class D, class VP, class EP, class GP, class EL>
inline void serialize(
    Archive & ar,
    boost::adjacency_list<OEL,VL,D,VP,EP,GP,EL> &graph,
    const unsigned int file_version
){
    boost::serialization::split_free(ar, graph, file_version);
}

}//serialization
}//boost


#endif // ADJ_LIST_SERIALIZE_HPP
