//
//=======================================================================
// Copyright 1997, 1998, 1999, 2000 University of Notre Dame.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================
//
#ifndef BOOST_GRAPH_UTILITY_HPP
#define BOOST_GRAPH_UTILITY_HPP

#include <stdlib.h>
#include <iostream>
#include <algorithm>
#include <assert.h>
#include <boost/config.hpp>
#include <boost/tuple/tuple.hpp>

#if !defined BOOST_NO_SLIST
#  ifdef BOOST_SLIST_HEADER
#    include BOOST_SLIST_HEADER
#  else
#    include <slist>
#  endif
#endif

#include <boost/graph/graph_traits.hpp>
#include <boost/graph/properties.hpp>
#include <boost/pending/container_traits.hpp>
#include <boost/graph/depth_first_search.hpp>
// iota moved to detail/algorithm.hpp
#include <boost/detail/algorithm.hpp>

namespace boost {

  // Provide an undirected graph interface alternative to the
  // the source() and target() edge functions.
  template <class UndirectedGraph>
  inline 
  std::pair<typename graph_traits<UndirectedGraph>::vertex_descriptor,
            typename graph_traits<UndirectedGraph>::vertex_descriptor>
  incident(typename graph_traits<UndirectedGraph>::edge_descriptor e,
           UndirectedGraph& g)
  {
    return std::make_pair(source(e,g), target(e,g));
  }

  // Provide an undirected graph interface alternative
  // to the out_edges() function.
  template <class Graph>
  inline 
  std::pair<typename graph_traits<Graph>::out_edge_iterator,
            typename graph_traits<Graph>::out_edge_iterator>
  incident_edges(typename graph_traits<Graph>::vertex_descriptor u,
                 Graph& g)
  {
    return out_edges(u, g);
  }

  template <class Graph>
  inline typename graph_traits<Graph>::vertex_descriptor
  opposite(typename graph_traits<Graph>::edge_descriptor e,
           typename graph_traits<Graph>::vertex_descriptor v,
           const Graph& g)
  {
    typedef typename graph_traits<Graph>::vertex_descriptor vertex_descriptor;
    if (v == source(e, g))
      return target(e, g);
    else if (v == target(e, g))
      return source(e, g);
    else
      return vertex_descriptor();
  }

  //===========================================================================
  // Some handy predicates

  template <typename Vertex, typename Graph>
  struct incident_from_predicate {
    incident_from_predicate(Vertex u, const Graph& g)
      : m_u(u), m_g(g) { }
    template <class Edge>
    bool operator()(const Edge& e) const {
      return source(e, m_g) == m_u;
    }
    Vertex m_u;
    const Graph& m_g;
  };
  template <typename Vertex, typename Graph>
  inline incident_from_predicate<Vertex, Graph>
  incident_from(Vertex u, const Graph& g) {
    return incident_from_predicate<Vertex, Graph>(u, g);
  }
  
  template <typename Vertex, typename Graph>
  struct incident_to_predicate {
    incident_to_predicate(Vertex u, const Graph& g)
      : m_u(u), m_g(g) { }
    template <class Edge>
    bool operator()(const Edge& e) const {
      return target(e, m_g) == m_u;
    }
    Vertex m_u;
    const Graph& m_g;
  };
  template <typename Vertex, typename Graph>
  inline incident_to_predicate<Vertex, Graph>
  incident_to(Vertex u, const Graph& g) {
    return incident_to_predicate<Vertex, Graph>(u, g);
  }

  template <typename Vertex, typename Graph>
  struct incident_on_predicate {
    incident_on_predicate(Vertex u, const Graph& g)
      : m_u(u), m_g(g) { }
    template <class Edge>
    bool operator()(const Edge& e) const {
      return source(e, m_g) == m_u || target(e, m_g) == m_u;
    }
    Vertex m_u;
    const Graph& m_g;
  };
  template <typename Vertex, typename Graph>
  inline incident_on_predicate<Vertex, Graph>
  incident_on(Vertex u, const Graph& g) {
    return incident_on_predicate<Vertex, Graph>(u, g);
  }
  
  template <typename Vertex, typename Graph>
  struct connects_predicate {
    connects_predicate(Vertex u, Vertex v, const Graph& g)
      : m_u(u), m_v(v), m_g(g) { }
    template <class Edge>
    bool operator()(const Edge& e) const {
      if (is_directed(m_g))
        return source(e, m_g) == m_u && target(e, m_g) == m_v;
      else
        return (source(e, m_g) == m_u && target(e, m_g) == m_v)
          || (source(e, m_g) == m_v && target(e, m_g) == m_u);
    }
    Vertex m_u, m_v;
    const Graph& m_g;
  };
  template <typename Vertex, typename Graph>
  inline connects_predicate<Vertex, Graph>
  connects(Vertex u, Vertex v, const Graph& g) {
          return connects_predicate<Vertex, Graph>(u, v, g);
  }


  // Need to convert all of these printing functions to take an ostream object
  // -JGS

  template <class IncidenceGraph, class Name>
  void print_in_edges(const IncidenceGraph& G, Name name)
  {
    typename graph_traits<IncidenceGraph>::vertex_iterator ui,ui_end;
    for (tie(ui,ui_end) = vertices(G); ui != ui_end; ++ui) {
      std::cout << get(name,*ui) << " <-- ";
      typename graph_traits<IncidenceGraph>
        ::in_edge_iterator ei, ei_end;
      for(tie(ei,ei_end) = in_edges(*ui,G); ei != ei_end; ++ei)
        std::cout << get(name,source(*ei,G)) << " ";
      std::cout << std::endl;
    }
  }

  template <class IncidenceGraph, class Name>
  void print_graph_dispatch(const IncidenceGraph& G, Name name, directed_tag)
  {
    typename graph_traits<IncidenceGraph>::vertex_iterator ui,ui_end;
    for (tie(ui,ui_end) = vertices(G); ui != ui_end; ++ui) {
      std::cout << get(name,*ui) << " --> ";
      typename graph_traits<IncidenceGraph>
        ::out_edge_iterator ei, ei_end;
      for(tie(ei,ei_end) = out_edges(*ui,G); ei != ei_end; ++ei)
        std::cout << get(name,target(*ei,G)) << " ";
      std::cout << std::endl;
    }
  }
  template <class IncidenceGraph, class Name>
  void print_graph_dispatch(const IncidenceGraph& G, Name name, undirected_tag)
  {
    typename graph_traits<IncidenceGraph>::vertex_iterator ui,ui_end;
    for (tie(ui,ui_end) = vertices(G); ui != ui_end; ++ui) {
      std::cout << get(name,*ui) << " <--> ";
      typename graph_traits<IncidenceGraph>
        ::out_edge_iterator ei, ei_end;
      for(tie(ei,ei_end) = out_edges(*ui,G); ei != ei_end; ++ei)
        std::cout << get(name,target(*ei,G)) << " ";
      std::cout << std::endl;
    }
  }
  template <class IncidenceGraph, class Name>
  void print_graph(const IncidenceGraph& G, Name name)
  {
    typedef typename graph_traits<IncidenceGraph>
      ::directed_category Cat;
    print_graph_dispatch(G, name, Cat());
  }
  template <class IncidenceGraph>
  void print_graph(const IncidenceGraph& G) {
    print_graph(G, get(vertex_index, G));
  }

  template <class EdgeListGraph, class Name>
  void print_edges(const EdgeListGraph& G, Name name)
  {
    typename graph_traits<EdgeListGraph>::edge_iterator ei, ei_end;
    for (tie(ei, ei_end) = edges(G); ei != ei_end; ++ei)
      std::cout << "(" << get(name, source(*ei, G))
                << "," << get(name, target(*ei, G)) << ") ";
    std::cout << std::endl;
  }

  template <class EdgeListGraph, class VertexName, class EdgeName>
  void print_edges2(const EdgeListGraph& G, VertexName vname, EdgeName ename)
  {
    typename graph_traits<EdgeListGraph>::edge_iterator ei, ei_end;
    for (tie(ei, ei_end) = edges(G); ei != ei_end; ++ei)
      std::cout << get(ename, *ei) << "(" << get(vname, source(*ei, G))
                << "," << get(vname, target(*ei, G)) << ") ";
    std::cout << std::endl;
  }

  template <class VertexListGraph, class Name>
  void print_vertices(const VertexListGraph& G, Name name)
  {
    typename graph_traits<VertexListGraph>::vertex_iterator vi,vi_end;
    for (tie(vi,vi_end) = vertices(G); vi != vi_end; ++vi)
      std::cout << get(name,*vi) << " ";
    std::cout << std::endl;
  }

  template <class Graph, class Vertex>
  bool is_adj_dispatch(Graph& g, Vertex a, Vertex b, bidirectional_tag)
  {
    typedef typename graph_traits<Graph>::edge_descriptor 
      edge_descriptor;
    typename graph_traits<Graph>::adjacency_iterator vi, viend, 
      adj_found;
    tie(vi, viend) = adjacent_vertices(a, g);
    adj_found = std::find(vi, viend, b);
    if (adj_found == viend)
      return false;  

    typename graph_traits<Graph>::out_edge_iterator oi, oiend, 
      out_found;
    tie(oi, oiend) = out_edges(a, g);
    out_found = std::find_if(oi, oiend, incident_to(b, g));
    if (out_found == oiend)
      return false;

    typename graph_traits<Graph>::in_edge_iterator ii, iiend, 
      in_found;
    tie(ii, iiend) = in_edges(b, g);
    in_found = std::find_if(ii, iiend, incident_from(a, g));
    if (in_found == iiend)
      return false;

    return true;
  }
  template <class Graph, class Vertex>
  bool is_adj_dispatch(Graph& g, Vertex a, Vertex b, directed_tag)
  {
    typedef typename graph_traits<Graph>::edge_descriptor 
      edge_descriptor;
    typename graph_traits<Graph>::adjacency_iterator vi, viend, found;
    tie(vi, viend) = adjacent_vertices(a, g);
#if defined(BOOST_MSVC) && BOOST_MSVC <= 1300 && defined(__SGI_STL_PORT)
    // Getting internal compiler error with std::find()
    found = viend;
    for (; vi != viend; ++vi)
      if (*vi == b) {
        found = vi;
        break;
      }
#else
    found = std::find(vi, viend, b);
#endif
    if ( found == viend )
      return false;

    typename graph_traits<Graph>::out_edge_iterator oi, oiend, 
      out_found;
    tie(oi, oiend) = out_edges(a, g);

#if defined(BOOST_MSVC) && BOOST_MSVC <= 1300 && defined(__SGI_STL_PORT)
    // Getting internal compiler error with std::find()
    out_found = oiend;
    for (; oi != oiend; ++oi)
      if (target(*oi, g) == b) {
        out_found = oi;
        break;
      }
#else
    out_found = std::find_if(oi, oiend, incident_to(b, g));
#endif
    if (out_found == oiend)
      return false;
    return true;
  }
  template <class Graph, class Vertex>
  bool is_adj_dispatch(Graph& g, Vertex a, Vertex b, undirected_tag)
  {
    return is_adj_dispatch(g, a, b, directed_tag());
  }

  template <class Graph, class Vertex>
  bool is_adjacent(Graph& g, Vertex a, Vertex b) {
    typedef typename graph_traits<Graph>::directed_category Cat;
    return is_adj_dispatch(g, a, b, Cat());
  }

  template <class Graph, class Edge>
  bool in_edge_set(Graph& g, Edge e)
  {
    typename Graph::edge_iterator ei, ei_end, found;
    tie(ei, ei_end) = edges(g);
    found = std::find(ei, ei_end, e);
    return found != ei_end;
  }

  template <class Graph, class Vertex>
  bool in_vertex_set(Graph& g, Vertex v)
  {
    typename Graph::vertex_iterator vi, vi_end, found;
    tie(vi, vi_end) = vertices(g);
    found = std::find(vi, vi_end, v);
    return found != vi_end;
  }

  template <class Graph, class Vertex>
  bool in_edge_set(Graph& g, Vertex u, Vertex v)
  {
    typename Graph::edge_iterator ei, ei_end;
    for (tie(ei,ei_end) = edges(g); ei != ei_end; ++ei)
      if (source(*ei,g) == u && target(*ei,g) == v)
        return true;
    return false;
  }

  // is x a descendant of y?
  template <typename ParentMap>
  inline bool is_descendant
  (typename property_traits<ParentMap>::value_type x,
   typename property_traits<ParentMap>::value_type y,
   ParentMap parent) 
  {
    if (get(parent, x) == x) // x is the root of the tree
      return false;
    else if (get(parent, x) == y)
      return true;
    else
      return is_descendant(get(parent, x), y, parent);
  }

  // is y reachable from x?
  template <typename IncidenceGraph, typename VertexColorMap>
  inline bool is_reachable
    (typename graph_traits<IncidenceGraph>::vertex_descriptor x,
     typename graph_traits<IncidenceGraph>::vertex_descriptor y,
     const IncidenceGraph& g,
     VertexColorMap color) // should start out white for every vertex
  {
    typedef typename property_traits<VertexColorMap>::value_type ColorValue;
    dfs_visitor<> vis;
    depth_first_visit(g, x, vis, color);
    return get(color, y) != color_traits<ColorValue>::white();
  }

  // Is the undirected graph connected?
  // Is the directed graph strongly connected?
  template <typename VertexListGraph, typename VertexColorMap>
  inline bool is_connected(const VertexListGraph& g, VertexColorMap color)
  {
    typedef typename property_traits<VertexColorMap>::value_type ColorValue;
    typedef color_traits<ColorValue> Color;
    typename graph_traits<VertexListGraph>::vertex_iterator 
      ui, ui_end, vi, vi_end, ci, ci_end;
    for (tie(ui, ui_end) = vertices(g); ui != ui_end; ++ui)
      for (tie(vi, vi_end) = vertices(g); vi != vi_end; ++vi)
        if (*ui != *vi) {
          for (tie(ci, ci_end) = vertices(g); ci != ci_end; ++ci) 
            put(color, *ci, Color::white());
          if (! is_reachable(*ui, *vi, color))
            return false;
        }
    return true;
  }

  template <typename Graph>
  bool is_self_loop
    (typename graph_traits<Graph>::edge_descriptor e,
     const Graph& g)
  {
    return source(e, g) == target(e, g);
  }


  template <class T1, class T2>
  std::pair<T1,T2> 
  make_list(const T1& t1, const T2& t2) 
    { return std::make_pair(t1, t2); }

  template <class T1, class T2, class T3>
  std::pair<T1,std::pair<T2,T3> > 
  make_list(const T1& t1, const T2& t2, const T3& t3)
    { return std::make_pair(t1, std::make_pair(t2, t3)); }

  template <class T1, class T2, class T3, class T4>
  std::pair<T1,std::pair<T2,std::pair<T3,T4> > > 
  make_list(const T1& t1, const T2& t2, const T3& t3, const T4& t4)
    { return std::make_pair(t1, std::make_pair(t2, std::make_pair(t3, t4))); }

  template <class T1, class T2, class T3, class T4, class T5>
  std::pair<T1,std::pair<T2,std::pair<T3,std::pair<T4,T5> > > > 
  make_list(const T1& t1, const T2& t2, const T3& t3, const T4& t4, const T5& t5)
    { return std::make_pair(t1, std::make_pair(t2, std::make_pair(t3, std::make_pair(t4, t5)))); }

} /* namespace boost */

#endif /* BOOST_GRAPH_UTILITY_HPP*/
