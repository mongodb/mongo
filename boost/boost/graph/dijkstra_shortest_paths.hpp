//=======================================================================
// Copyright 1997, 1998, 1999, 2000 University of Notre Dame.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================
#ifndef BOOST_GRAPH_DIJKSTRA_HPP
#define BOOST_GRAPH_DIJKSTRA_HPP

#include <functional>
#include <boost/limits.hpp>
#include <boost/graph/named_function_params.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/graph/relax.hpp>
#include <boost/pending/indirect_cmp.hpp>
#include <boost/graph/exception.hpp>
#include <boost/pending/relaxed_heap.hpp>

#ifdef BOOST_GRAPH_DIJKSTRA_TESTING
#  include <boost/pending/mutable_queue.hpp>
#endif // BOOST_GRAPH_DIJKSTRA_TESTING

namespace boost {

#ifdef BOOST_GRAPH_DIJKSTRA_TESTING
  static bool dijkstra_relaxed_heap = true;
#endif

  template <class Visitor, class Graph>
  struct DijkstraVisitorConcept {
    void constraints() {
      function_requires< CopyConstructibleConcept<Visitor> >();
      vis.initialize_vertex(u, g);
      vis.discover_vertex(u, g);
      vis.examine_vertex(u, g);
      vis.examine_edge(e, g);
      vis.edge_relaxed(e, g);
      vis.edge_not_relaxed(e, g);
      vis.finish_vertex(u, g);
    }
    Visitor vis;
    Graph g;
    typename graph_traits<Graph>::vertex_descriptor u;
    typename graph_traits<Graph>::edge_descriptor e;
  };

  template <class Visitors = null_visitor>
  class dijkstra_visitor : public bfs_visitor<Visitors> {
  public:
    dijkstra_visitor() { }
    dijkstra_visitor(Visitors vis)
      : bfs_visitor<Visitors>(vis) { }

    template <class Edge, class Graph>
    void edge_relaxed(Edge e, Graph& g) {
      invoke_visitors(this->m_vis, e, g, on_edge_relaxed());
    }
    template <class Edge, class Graph>
    void edge_not_relaxed(Edge e, Graph& g) {
      invoke_visitors(this->m_vis, e, g, on_edge_not_relaxed());
    }
  private:
    template <class Edge, class Graph>
    void tree_edge(Edge u, Graph& g) { }
  };
  template <class Visitors>
  dijkstra_visitor<Visitors>
  make_dijkstra_visitor(Visitors vis) {
    return dijkstra_visitor<Visitors>(vis);
  }
  typedef dijkstra_visitor<> default_dijkstra_visitor;

  namespace detail {

    template <class UniformCostVisitor, class UpdatableQueue,
      class WeightMap, class PredecessorMap, class DistanceMap,
      class BinaryFunction, class BinaryPredicate>
    struct dijkstra_bfs_visitor
    {
      typedef typename property_traits<DistanceMap>::value_type D;

      dijkstra_bfs_visitor(UniformCostVisitor vis, UpdatableQueue& Q,
                           WeightMap w, PredecessorMap p, DistanceMap d,
                           BinaryFunction combine, BinaryPredicate compare,
                           D zero)
        : m_vis(vis), m_Q(Q), m_weight(w), m_predecessor(p), m_distance(d),
          m_combine(combine), m_compare(compare), m_zero(zero)  { }

      template <class Edge, class Graph>
      void tree_edge(Edge e, Graph& g) {
        m_decreased = relax(e, g, m_weight, m_predecessor, m_distance,
                            m_combine, m_compare);
        if (m_decreased)
          m_vis.edge_relaxed(e, g);
        else
          m_vis.edge_not_relaxed(e, g);
      }
      template <class Edge, class Graph>
      void gray_target(Edge e, Graph& g) {
        m_decreased = relax(e, g, m_weight, m_predecessor, m_distance,
                            m_combine, m_compare);
        if (m_decreased) {
          m_Q.update(target(e, g));
          m_vis.edge_relaxed(e, g);
        } else
          m_vis.edge_not_relaxed(e, g);
      }

      template <class Vertex, class Graph>
      void initialize_vertex(Vertex u, Graph& g) { }
      template <class Edge, class Graph>
      void non_tree_edge(Edge, Graph&) { }
      template <class Vertex, class Graph>
      void discover_vertex(Vertex u, Graph& g) { m_vis.discover_vertex(u, g); }
      template <class Vertex, class Graph>
      void examine_vertex(Vertex u, Graph& g) { m_vis.examine_vertex(u, g); }
      template <class Edge, class Graph>
      void examine_edge(Edge e, Graph& g) {
        if (m_compare(get(m_weight, e), m_zero))
          throw negative_edge();
        m_vis.examine_edge(e, g);
      }
      template <class Edge, class Graph>
      void black_target(Edge, Graph&) { }
      template <class Vertex, class Graph>
      void finish_vertex(Vertex u, Graph& g) { m_vis.finish_vertex(u, g); }

      UniformCostVisitor m_vis;
      UpdatableQueue& m_Q;
      WeightMap m_weight;
      PredecessorMap m_predecessor;
      DistanceMap m_distance;
      BinaryFunction m_combine;
      BinaryPredicate m_compare;
      bool m_decreased;
      D m_zero;
    };

  } // namespace detail

  // Call breadth first search with default color map.
  template <class VertexListGraph, class DijkstraVisitor,
            class PredecessorMap, class DistanceMap,
            class WeightMap, class IndexMap, class Compare, class Combine,
            class DistZero>
  inline void
  dijkstra_shortest_paths_no_init
    (const VertexListGraph& g,
     typename graph_traits<VertexListGraph>::vertex_descriptor s,
     PredecessorMap predecessor, DistanceMap distance, WeightMap weight,
     IndexMap index_map,
     Compare compare, Combine combine, DistZero zero,
     DijkstraVisitor vis)
  {
    std::vector<default_color_type> color(num_vertices(g));
    default_color_type c = white_color;
    dijkstra_shortest_paths_no_init( g, s, predecessor, distance, weight,
      index_map, compare, combine, zero, vis,
        make_iterator_property_map(&color[0], index_map, c));
  }

  // Call breadth first search
  template <class VertexListGraph, class DijkstraVisitor,
            class PredecessorMap, class DistanceMap,
            class WeightMap, class IndexMap, class Compare, class Combine,
            class DistZero, class ColorMap>
  inline void
  dijkstra_shortest_paths_no_init
    (const VertexListGraph& g,
     typename graph_traits<VertexListGraph>::vertex_descriptor s,
     PredecessorMap predecessor, DistanceMap distance, WeightMap weight,
     IndexMap index_map,
     Compare compare, Combine combine, DistZero zero,
     DijkstraVisitor vis, ColorMap color)
  {
    typedef indirect_cmp<DistanceMap, Compare> IndirectCmp;
    IndirectCmp icmp(distance, compare);

    typedef typename graph_traits<VertexListGraph>::vertex_descriptor Vertex;

#ifdef BOOST_GRAPH_DIJKSTRA_TESTING
    if (!dijkstra_relaxed_heap) {
      typedef mutable_queue<Vertex, std::vector<Vertex>, IndirectCmp, IndexMap>
        MutableQueue;

      MutableQueue Q(num_vertices(g), icmp, index_map);

      detail::dijkstra_bfs_visitor<DijkstraVisitor, MutableQueue, WeightMap,
        PredecessorMap, DistanceMap, Combine, Compare>
      bfs_vis(vis, Q, weight, predecessor, distance, combine, compare, zero);

      breadth_first_visit(g, s, Q, bfs_vis, color);
      return;
    }
#endif // BOOST_GRAPH_DIJKSTRA_TESTING

    typedef relaxed_heap<Vertex, IndirectCmp, IndexMap> MutableQueue;

    MutableQueue Q(num_vertices(g), icmp, index_map);

    detail::dijkstra_bfs_visitor<DijkstraVisitor, MutableQueue, WeightMap,
      PredecessorMap, DistanceMap, Combine, Compare>
        bfs_vis(vis, Q, weight, predecessor, distance, combine, compare, zero);

    breadth_first_visit(g, s, Q, bfs_vis, color);
  }

  // Initialize distances and call breadth first search with default color map
  template <class VertexListGraph, class DijkstraVisitor,
            class PredecessorMap, class DistanceMap,
            class WeightMap, class IndexMap, class Compare, class Combine,
            class DistInf, class DistZero>
  inline void
  dijkstra_shortest_paths
    (const VertexListGraph& g,
     typename graph_traits<VertexListGraph>::vertex_descriptor s,
     PredecessorMap predecessor, DistanceMap distance, WeightMap weight,
     IndexMap index_map,
     Compare compare, Combine combine, DistInf inf, DistZero zero,
     DijkstraVisitor vis)
  {
    std::vector<default_color_type> color(num_vertices(g));
    default_color_type c = white_color;
    dijkstra_shortest_paths(g, s, predecessor, distance, weight, index_map,
                            compare, combine, inf, zero, vis,
                            make_iterator_property_map(&color[0], index_map,
                                                       c));
  }

  // Initialize distances and call breadth first search
  template <class VertexListGraph, class DijkstraVisitor,
            class PredecessorMap, class DistanceMap,
            class WeightMap, class IndexMap, class Compare, class Combine,
            class DistInf, class DistZero, class ColorMap>
  inline void
  dijkstra_shortest_paths
    (const VertexListGraph& g,
     typename graph_traits<VertexListGraph>::vertex_descriptor s,
     PredecessorMap predecessor, DistanceMap distance, WeightMap weight,
     IndexMap index_map,
     Compare compare, Combine combine, DistInf inf, DistZero zero,
     DijkstraVisitor vis, ColorMap color)
  {
    typedef typename property_traits<ColorMap>::value_type ColorValue;
    typedef color_traits<ColorValue> Color;
    typename graph_traits<VertexListGraph>::vertex_iterator ui, ui_end;
    for (tie(ui, ui_end) = vertices(g); ui != ui_end; ++ui) {
      vis.initialize_vertex(*ui, g);
      put(distance, *ui, inf);
      put(predecessor, *ui, *ui);
      put(color, *ui, Color::white());
    }
    put(distance, s, zero);

    dijkstra_shortest_paths_no_init(g, s, predecessor, distance, weight,
                            index_map, compare, combine, zero, vis, color);
  }

  namespace detail {

    // Handle defaults for PredecessorMap and
    // Distance Compare, Combine, Inf and Zero
    template <class VertexListGraph, class DistanceMap, class WeightMap,
              class IndexMap, class Params, class ColorMap>
    inline void
    dijkstra_dispatch2
      (const VertexListGraph& g,
       typename graph_traits<VertexListGraph>::vertex_descriptor s,
       DistanceMap distance, WeightMap weight, IndexMap index_map,
       const Params& params, ColorMap color)
    {
      // Default for predecessor map
      dummy_property_map p_map;

      typedef typename property_traits<DistanceMap>::value_type D;
      dijkstra_shortest_paths
        (g, s,
         choose_param(get_param(params, vertex_predecessor), p_map),
         distance, weight, index_map,
         choose_param(get_param(params, distance_compare_t()),
                      std::less<D>()),
         choose_param(get_param(params, distance_combine_t()),
                      closed_plus<D>()),
         choose_param(get_param(params, distance_inf_t()),
                      (std::numeric_limits<D>::max)()),
         choose_param(get_param(params, distance_zero_t()),
                      D()),
         choose_param(get_param(params, graph_visitor),
                      make_dijkstra_visitor(null_visitor())),
         color);
    }

    template <class VertexListGraph, class DistanceMap, class WeightMap,
              class IndexMap, class Params, class ColorMap>
    inline void
    dijkstra_dispatch1
      (const VertexListGraph& g,
       typename graph_traits<VertexListGraph>::vertex_descriptor s,
       DistanceMap distance, WeightMap weight, IndexMap index_map,
       const Params& params, ColorMap color)
    {
      // Default for distance map
      typedef typename property_traits<WeightMap>::value_type D;
      typename std::vector<D>::size_type
        n = is_default_param(distance) ? num_vertices(g) : 1;
      std::vector<D> distance_map(n);

      // Default for color map
      typename std::vector<default_color_type>::size_type
        m = is_default_param(color) ? num_vertices(g) : 1;
      std::vector<default_color_type> color_map(m);

      detail::dijkstra_dispatch2
        (g, s, choose_param(distance, make_iterator_property_map
                            (distance_map.begin(), index_map,
                             distance_map[0])),
         weight, index_map, params,
         choose_param(color, make_iterator_property_map
                      (color_map.begin(), index_map,
                       color_map[0])));
    }
  } // namespace detail

  // Named Parameter Variant
  template <class VertexListGraph, class Param, class Tag, class Rest>
  inline void
  dijkstra_shortest_paths
    (const VertexListGraph& g,
     typename graph_traits<VertexListGraph>::vertex_descriptor s,
     const bgl_named_params<Param,Tag,Rest>& params)
  {
    // Default for edge weight and vertex index map is to ask for them
    // from the graph.  Default for the visitor is null_visitor.
    detail::dijkstra_dispatch1
      (g, s,
       get_param(params, vertex_distance),
       choose_const_pmap(get_param(params, edge_weight), g, edge_weight),
       choose_const_pmap(get_param(params, vertex_index), g, vertex_index),
       params,
       get_param(params, vertex_color));
  }

} // namespace boost

#endif // BOOST_GRAPH_DIJKSTRA_HPP
