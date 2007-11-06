

//
//=======================================================================
// Copyright (c) 2004 Kristopher Beevers
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================
//

#ifndef BOOST_GRAPH_ASTAR_SEARCH_HPP
#define BOOST_GRAPH_ASTAR_SEARCH_HPP


#include <functional>
#include <boost/limits.hpp>
#include <boost/graph/named_function_params.hpp>
#include <boost/pending/mutable_queue.hpp>
#include <boost/graph/relax.hpp>
#include <boost/pending/indirect_cmp.hpp>
#include <boost/graph/exception.hpp>
#include <boost/graph/breadth_first_search.hpp>


namespace boost {

  
  template <class Heuristic, class Graph>
  struct AStarHeuristicConcept {
    void constraints()
    {
      function_requires< CopyConstructibleConcept<Heuristic> >();
      h(u);
    }
    Heuristic h;
    typename graph_traits<Graph>::vertex_descriptor u;
  };
  
  
  template <class Graph, class CostType>
  class astar_heuristic : public std::unary_function<
    typename graph_traits<Graph>::vertex_descriptor, CostType>
  {
  public:
    typedef typename graph_traits<Graph>::vertex_descriptor Vertex;
    astar_heuristic() {}
    CostType operator()(Vertex u) { return static_cast<CostType>(0); }
  };
  

  
  template <class Visitor, class Graph>
  struct AStarVisitorConcept {
    void constraints()
    {
      function_requires< CopyConstructibleConcept<Visitor> >();
      vis.initialize_vertex(u, g);
      vis.discover_vertex(u, g);
      vis.examine_vertex(u, g);
      vis.examine_edge(e, g);
      vis.edge_relaxed(e, g);
      vis.edge_not_relaxed(e, g);
      vis.black_target(e, g);
      vis.finish_vertex(u, g);
    }
    Visitor vis;
    Graph g;
    typename graph_traits<Graph>::vertex_descriptor u;
    typename graph_traits<Graph>::edge_descriptor e;
  };
  
  
  template <class Visitors = null_visitor>
  class astar_visitor : public bfs_visitor<Visitors> {
  public:
    astar_visitor() {}
    astar_visitor(Visitors vis)
      : bfs_visitor<Visitors>(vis) {}
  
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
    void tree_edge(Edge e, Graph& g) {}
    template <class Edge, class Graph>
    void non_tree_edge(Edge e, Graph& g) {}
  };
  template <class Visitors>
  astar_visitor<Visitors>
  make_astar_visitor(Visitors vis) {
    return astar_visitor<Visitors>(vis);
  }
  typedef astar_visitor<> default_astar_visitor;
  

  namespace detail {
    
    template <class AStarHeuristic, class UniformCostVisitor,
              class UpdatableQueue, class PredecessorMap,
              class CostMap, class DistanceMap, class WeightMap,
              class ColorMap, class BinaryFunction,
              class BinaryPredicate>
    struct astar_bfs_visitor
    {
      
      typedef typename property_traits<CostMap>::value_type C;
      typedef typename property_traits<ColorMap>::value_type ColorValue;
      typedef color_traits<ColorValue> Color;
      typedef typename property_traits<DistanceMap>::value_type distance_type;
      
      astar_bfs_visitor(AStarHeuristic h, UniformCostVisitor vis,
                        UpdatableQueue& Q, PredecessorMap p,
                        CostMap c, DistanceMap d, WeightMap w,
                        ColorMap col, BinaryFunction combine,
                        BinaryPredicate compare, C zero)
        : m_h(h), m_vis(vis), m_Q(Q), m_predecessor(p), m_cost(c),
          m_distance(d), m_weight(w), m_color(col),
          m_combine(combine), m_compare(compare), m_zero(zero) {}
      
      
      template <class Vertex, class Graph>
      void initialize_vertex(Vertex u, Graph& g) {
        m_vis.initialize_vertex(u, g);
      }
      template <class Vertex, class Graph>
      void discover_vertex(Vertex u, Graph& g) {
        m_vis.discover_vertex(u, g);
      }
      template <class Vertex, class Graph>
      void examine_vertex(Vertex u, Graph& g) {
        m_vis.examine_vertex(u, g);
      }
      template <class Vertex, class Graph>
      void finish_vertex(Vertex u, Graph& g) {
        m_vis.finish_vertex(u, g);
      }
      template <class Edge, class Graph>
      void examine_edge(Edge e, Graph& g) { 
        if (m_compare(get(m_weight, e), m_zero))
          throw negative_edge();
        m_vis.examine_edge(e, g);
      }
      template <class Edge, class Graph>
      void non_tree_edge(Edge, Graph&) {}
      
      
      
      template <class Edge, class Graph>
      void tree_edge(Edge e, Graph& g) {
        m_decreased = relax(e, g, m_weight, m_predecessor, m_distance,
                            m_combine, m_compare);

        if(m_decreased) {
          m_vis.edge_relaxed(e, g);
          put(m_cost, target(e, g),
              m_combine(get(m_distance, target(e, g)),
                        m_h(target(e, g))));
        } else
          m_vis.edge_not_relaxed(e, g);
      }
      
      
      template <class Edge, class Graph>
      void gray_target(Edge e, Graph& g) {
        distance_type old_distance = get(m_distance, target(e, g));

        m_decreased = relax(e, g, m_weight, m_predecessor, m_distance,
                            m_combine, m_compare);

        /* On x86 Linux with optimization, we sometimes get into a
           horrible case where m_decreased is true but the distance hasn't
           actually changed. This occurs when the comparison inside
           relax() occurs with the 80-bit precision of the x87 floating
           point unit, but the difference is lost when the resulting
           values are written back to lower-precision memory (e.g., a
           double). With the eager Dijkstra's implementation, this results
           in looping. */
        if(m_decreased && old_distance != get(m_distance, target(e, g))) {
          put(m_cost, target(e, g),
              m_combine(get(m_distance, target(e, g)),
                        m_h(target(e, g))));
          m_Q.update(target(e, g));
          m_vis.edge_relaxed(e, g);
        } else
          m_vis.edge_not_relaxed(e, g);
      }
      
      
      template <class Edge, class Graph>
      void black_target(Edge e, Graph& g) {
        distance_type old_distance = get(m_distance, target(e, g));

        m_decreased = relax(e, g, m_weight, m_predecessor, m_distance,
                            m_combine, m_compare);

        /* See comment in gray_target */
        if(m_decreased && old_distance != get(m_distance, target(e, g))) {
          m_vis.edge_relaxed(e, g);
          put(m_cost, target(e, g),
              m_combine(get(m_distance, target(e, g)),
                        m_h(target(e, g))));
          m_Q.push(target(e, g));
          put(m_color, target(e, g), Color::gray());
          m_vis.black_target(e, g);
        } else
          m_vis.edge_not_relaxed(e, g);
      }
      
      
      
      AStarHeuristic m_h;
      UniformCostVisitor m_vis;
      UpdatableQueue& m_Q;
      PredecessorMap m_predecessor;
      CostMap m_cost;
      DistanceMap m_distance;
      WeightMap m_weight;
      ColorMap m_color;
      BinaryFunction m_combine;
      BinaryPredicate m_compare;
      bool m_decreased;
      C m_zero;
      
    };
    
  } // namespace detail

  
  
  template <typename VertexListGraph, typename AStarHeuristic,
            typename AStarVisitor, typename PredecessorMap,
            typename CostMap, typename DistanceMap,
            typename WeightMap, typename ColorMap,
            typename VertexIndexMap,
            typename CompareFunction, typename CombineFunction,
            typename CostInf, typename CostZero>
  inline void
  astar_search_no_init
    (VertexListGraph &g,
     typename graph_traits<VertexListGraph>::vertex_descriptor s,
     AStarHeuristic h, AStarVisitor vis,
     PredecessorMap predecessor, CostMap cost,
     DistanceMap distance, WeightMap weight,
     ColorMap color, VertexIndexMap index_map,
     CompareFunction compare, CombineFunction combine,
     CostInf inf, CostZero zero)
  {
    typedef indirect_cmp<CostMap, CompareFunction> IndirectCmp;
    IndirectCmp icmp(cost, compare);
  
    typedef typename graph_traits<VertexListGraph>::vertex_descriptor
      Vertex;
    typedef mutable_queue<Vertex, std::vector<Vertex>,
        IndirectCmp, VertexIndexMap>
      MutableQueue;
    MutableQueue Q(num_vertices(g), icmp, index_map);
  
    detail::astar_bfs_visitor<AStarHeuristic, AStarVisitor,
        MutableQueue, PredecessorMap, CostMap, DistanceMap,
        WeightMap, ColorMap, CombineFunction, CompareFunction>
      bfs_vis(h, vis, Q, predecessor, cost, distance, weight,
              color, combine, compare, zero);
  
    breadth_first_visit(g, s, Q, bfs_vis, color);
  }
  
  
  // Non-named parameter interface
  template <typename VertexListGraph, typename AStarHeuristic,
            typename AStarVisitor, typename PredecessorMap,
            typename CostMap, typename DistanceMap,
            typename WeightMap, typename VertexIndexMap,
            typename ColorMap,
            typename CompareFunction, typename CombineFunction,
            typename CostInf, typename CostZero>
  inline void
  astar_search
    (VertexListGraph &g,
     typename graph_traits<VertexListGraph>::vertex_descriptor s,
     AStarHeuristic h, AStarVisitor vis,
     PredecessorMap predecessor, CostMap cost,
     DistanceMap distance, WeightMap weight,
     VertexIndexMap index_map, ColorMap color,
     CompareFunction compare, CombineFunction combine,
     CostInf inf, CostZero zero)
  {
    
    typedef typename property_traits<ColorMap>::value_type ColorValue;
    typedef color_traits<ColorValue> Color;
    typename graph_traits<VertexListGraph>::vertex_iterator ui, ui_end;
    for (tie(ui, ui_end) = vertices(g); ui != ui_end; ++ui) {
      put(color, *ui, Color::white());
      put(distance, *ui, inf);
      put(cost, *ui, inf);
      put(predecessor, *ui, *ui);
      vis.initialize_vertex(*ui, g);
    }
    put(distance, s, zero);
    put(cost, s, h(s));
    
    astar_search_no_init
      (g, s, h, vis, predecessor, cost, distance, weight,
       color, index_map, compare, combine, inf, zero);
    
  }
  
  
  
  namespace detail {
    template <class VertexListGraph, class AStarHeuristic,
              class CostMap, class DistanceMap, class WeightMap,
              class IndexMap, class ColorMap, class Params>
    inline void
    astar_dispatch2
      (VertexListGraph& g,
       typename graph_traits<VertexListGraph>::vertex_descriptor s,
       AStarHeuristic h, CostMap cost, DistanceMap distance,
       WeightMap weight, IndexMap index_map, ColorMap color,
       const Params& params)
    {
      dummy_property_map p_map;
      typedef typename property_traits<CostMap>::value_type C;
      astar_search
        (g, s, h,
         choose_param(get_param(params, graph_visitor),
                      make_astar_visitor(null_visitor())),
         choose_param(get_param(params, vertex_predecessor), p_map),
         cost, distance, weight, index_map, color,
         choose_param(get_param(params, distance_compare_t()),
                      std::less<C>()),
         choose_param(get_param(params, distance_combine_t()),
                      closed_plus<C>()),
         choose_param(get_param(params, distance_inf_t()),
                      std::numeric_limits<C>::max BOOST_PREVENT_MACRO_SUBSTITUTION ()),
         choose_param(get_param(params, distance_zero_t()),
                      C()));
    }
  
    template <class VertexListGraph, class AStarHeuristic,
              class CostMap, class DistanceMap, class WeightMap,
              class IndexMap, class ColorMap, class Params>
    inline void
    astar_dispatch1
      (VertexListGraph& g,
       typename graph_traits<VertexListGraph>::vertex_descriptor s,
       AStarHeuristic h, CostMap cost, DistanceMap distance,
       WeightMap weight, IndexMap index_map, ColorMap color,
       const Params& params)
    {
      typedef typename property_traits<WeightMap>::value_type D;
      typename std::vector<D>::size_type
        n = is_default_param(distance) ? num_vertices(g) : 1;
      std::vector<D> distance_map(n);
      n = is_default_param(cost) ? num_vertices(g) : 1;
      std::vector<D> cost_map(n);
      std::vector<default_color_type> color_map(num_vertices(g));
      default_color_type c = white_color;
  
      detail::astar_dispatch2
        (g, s, h,
         choose_param(cost, make_iterator_property_map
                      (cost_map.begin(), index_map,
                       cost_map[0])),
         choose_param(distance, make_iterator_property_map
                      (distance_map.begin(), index_map, 
                       distance_map[0])),
         weight, index_map,
         choose_param(color, make_iterator_property_map
                      (color_map.begin(), index_map, c)),
         params);
    }
  } // namespace detail
  
  
  // Named parameter interface
  template <typename VertexListGraph,
            typename AStarHeuristic,
            typename P, typename T, typename R>
  void
  astar_search
    (VertexListGraph &g,
     typename graph_traits<VertexListGraph>::vertex_descriptor s,
     AStarHeuristic h, const bgl_named_params<P, T, R>& params)
  {
    
    detail::astar_dispatch1
      (g, s, h,
       get_param(params, vertex_rank),
       get_param(params, vertex_distance),
       choose_const_pmap(get_param(params, edge_weight), g, edge_weight),
       choose_const_pmap(get_param(params, vertex_index), g, vertex_index),
       get_param(params, vertex_color),
       params);
    
  }
  
} // namespace boost

#endif // BOOST_GRAPH_ASTAR_SEARCH_HPP
