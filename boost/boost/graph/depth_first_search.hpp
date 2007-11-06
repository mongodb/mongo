//=======================================================================
// Copyright 1997, 1998, 1999, 2000 University of Notre Dame.
// Copyright 2003 Bruce Barr
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

// Nonrecursive implementation of depth_first_visit_impl submitted by
// Bruce Barr, schmoost <at> yahoo.com, May/June 2003.
#ifndef BOOST_GRAPH_RECURSIVE_DFS_HPP
#define BOOST_GRAPH_RECURSIVE_DFS_HPP

#include <boost/config.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/graph_concepts.hpp>
#include <boost/graph/properties.hpp>
#include <boost/graph/visitors.hpp>
#include <boost/graph/named_function_params.hpp>

#include <boost/ref.hpp>
#include <boost/implicit_cast.hpp>

#include <vector>
#include <utility>

namespace boost {

  template <class Visitor, class Graph>
  class DFSVisitorConcept {
  public:
    void constraints() {
      function_requires< CopyConstructibleConcept<Visitor> >();
      vis.initialize_vertex(u, g);
      vis.start_vertex(u, g);
      vis.discover_vertex(u, g);
      vis.examine_edge(e, g);
      vis.tree_edge(e, g);
      vis.back_edge(e, g);
      vis.forward_or_cross_edge(e, g);
      vis.finish_vertex(u, g);
    }
  private:
    Visitor vis;
    Graph g;
    typename graph_traits<Graph>::vertex_descriptor u;
    typename graph_traits<Graph>::edge_descriptor e;
  };

  namespace detail {

    struct nontruth2 {
      template<class T, class T2>
      bool operator()(const T&, const T2&) const { return false; }
    };


// Define BOOST_RECURSIVE_DFS to use older, recursive version.
// It is retained for a while in order to perform performance
// comparison.
#ifndef BOOST_RECURSIVE_DFS

    // If the vertex u and the iterators ei and ei_end are thought of as the
    // context of the algorithm, each push and pop from the stack could
    // be thought of as a context shift.
    // Each pass through "while (ei != ei_end)" may refer to the out-edges of
    // an entirely different vertex, because the context of the algorithm
    // shifts every time a white adjacent vertex is discovered.
    // The corresponding context shift back from the adjacent vertex occurs
    // after all of its out-edges have been examined.
    //
    // See http://lists.boost.org/MailArchives/boost/msg48752.php for FAQ.

    template <class IncidenceGraph, class DFSVisitor, class ColorMap,
            class TerminatorFunc>
    void depth_first_visit_impl
      (const IncidenceGraph& g,
       typename graph_traits<IncidenceGraph>::vertex_descriptor u,
       DFSVisitor& vis,
       ColorMap color, TerminatorFunc func = TerminatorFunc())
    {
      function_requires<IncidenceGraphConcept<IncidenceGraph> >();
      function_requires<DFSVisitorConcept<DFSVisitor, IncidenceGraph> >();
      typedef typename graph_traits<IncidenceGraph>::vertex_descriptor Vertex;
      function_requires< ReadWritePropertyMapConcept<ColorMap, Vertex> >();
      typedef typename property_traits<ColorMap>::value_type ColorValue;
      function_requires< ColorValueConcept<ColorValue> >();
      typedef color_traits<ColorValue> Color;
      typedef typename graph_traits<IncidenceGraph>::out_edge_iterator Iter;
      typedef std::pair<Vertex, std::pair<Iter, Iter> > VertexInfo;

      Iter ei, ei_end;
      std::vector<VertexInfo> stack;

      // Possible optimization for vector
      //stack.reserve(num_vertices(g));

      typedef typename unwrap_reference<TerminatorFunc>::type TF;

      put(color, u, Color::gray());
      vis.discover_vertex(u, g);
      tie(ei, ei_end) = out_edges(u, g);
      // Variable is needed to workaround a borland bug.
      TF& fn = static_cast<TF&>(func);
      if (fn(u, g)) {
          // If this vertex terminates the search, we push empty range
          stack.push_back(std::make_pair(u, std::make_pair(ei_end, ei_end)));
      } else {
          stack.push_back(std::make_pair(u, std::make_pair(ei, ei_end)));
      }
      while (!stack.empty()) {
        VertexInfo& back = stack.back();
        u = back.first;
        tie(ei, ei_end) = back.second;
        stack.pop_back();
        while (ei != ei_end) {
          Vertex v = target(*ei, g);
          vis.examine_edge(*ei, g);
          ColorValue v_color = get(color, v);
          if (v_color == Color::white()) {
            vis.tree_edge(*ei, g);
            stack.push_back(std::make_pair(u, std::make_pair(++ei, ei_end)));
            u = v;
            put(color, u, Color::gray());
            vis.discover_vertex(u, g);
            tie(ei, ei_end) = out_edges(u, g);
            if (fn(u, g)) {
                ei = ei_end;
            }
          } else if (v_color == Color::gray()) {
            vis.back_edge(*ei, g);
            ++ei;
          } else {
            vis.forward_or_cross_edge(*ei, g);
            ++ei;
          }
        }
        put(color, u, Color::black());
        vis.finish_vertex(u, g);
      }
    }

#else // BOOST_RECURSIVE_DFS is defined

    template <class IncidenceGraph, class DFSVisitor, class ColorMap,
              class TerminatorFunc>
    void depth_first_visit_impl
      (const IncidenceGraph& g,
       typename graph_traits<IncidenceGraph>::vertex_descriptor u,
       DFSVisitor& vis,  // pass-by-reference here, important!
       ColorMap color, TerminatorFunc func)
    {
      function_requires<IncidenceGraphConcept<IncidenceGraph> >();
      function_requires<DFSVisitorConcept<DFSVisitor, IncidenceGraph> >();
      typedef typename graph_traits<IncidenceGraph>::vertex_descriptor Vertex;
      function_requires< ReadWritePropertyMapConcept<ColorMap, Vertex> >();
      typedef typename property_traits<ColorMap>::value_type ColorValue;
      function_requires< ColorValueConcept<ColorValue> >();
      typedef color_traits<ColorValue> Color;
      typename graph_traits<IncidenceGraph>::out_edge_iterator ei, ei_end;

      put(color, u, Color::gray());          vis.discover_vertex(u, g);

      typedef typename unwrap_reference<TerminatorFunc>::type TF;
      // Variable is needed to workaround a borland bug.
      TF& fn = static_cast<TF&>(func);
      if (!fn(u, g))
        for (tie(ei, ei_end) = out_edges(u, g); ei != ei_end; ++ei) {
          Vertex v = target(*ei, g);           vis.examine_edge(*ei, g);
          ColorValue v_color = get(color, v);
          if (v_color == Color::white()) {     vis.tree_edge(*ei, g);
          depth_first_visit_impl(g, v, vis, color, func);
          } else if (v_color == Color::gray()) vis.back_edge(*ei, g);
          else                                 vis.forward_or_cross_edge(*ei, g);
        }
      put(color, u, Color::black());         vis.finish_vertex(u, g);
    }

#endif

  } // namespace detail

  template <class VertexListGraph, class DFSVisitor, class ColorMap>
  void
  depth_first_search(const VertexListGraph& g, DFSVisitor vis, ColorMap color,
                     typename graph_traits<VertexListGraph>::vertex_descriptor start_vertex)
  {
    typedef typename graph_traits<VertexListGraph>::vertex_descriptor Vertex;
    function_requires<DFSVisitorConcept<DFSVisitor, VertexListGraph> >();
    typedef typename property_traits<ColorMap>::value_type ColorValue;
    typedef color_traits<ColorValue> Color;

    typename graph_traits<VertexListGraph>::vertex_iterator ui, ui_end;
    for (tie(ui, ui_end) = vertices(g); ui != ui_end; ++ui) {
      put(color, *ui, Color::white());       vis.initialize_vertex(*ui, g);
    }

    if (start_vertex != implicit_cast<Vertex>(*vertices(g).first)){ vis.start_vertex(start_vertex, g);
      detail::depth_first_visit_impl(g, start_vertex, vis, color,
                                     detail::nontruth2());
    }

    for (tie(ui, ui_end) = vertices(g); ui != ui_end; ++ui) {
      ColorValue u_color = get(color, *ui);
      if (u_color == Color::white()) {       vis.start_vertex(*ui, g);
        detail::depth_first_visit_impl(g, *ui, vis, color, detail::nontruth2());
      }
    }
  }

  template <class VertexListGraph, class DFSVisitor, class ColorMap>
  void
  depth_first_search(const VertexListGraph& g, DFSVisitor vis, ColorMap color)
  {
    if (vertices(g).first == vertices(g).second)
      return;

    depth_first_search(g, vis, color, *vertices(g).first);
  }

  namespace detail {
    template <class ColorMap>
    struct dfs_dispatch {

      template <class VertexListGraph, class Vertex, class DFSVisitor,
                class P, class T, class R>
      static void
      apply(const VertexListGraph& g, DFSVisitor vis, Vertex start_vertex,
            const bgl_named_params<P, T, R>&,
            ColorMap color)
      {
        depth_first_search(g, vis, color, start_vertex);
      }
    };

    template <>
    struct dfs_dispatch<detail::error_property_not_found> {
      template <class VertexListGraph, class Vertex, class DFSVisitor,
                class P, class T, class R>
      static void
      apply(const VertexListGraph& g, DFSVisitor vis, Vertex start_vertex,
            const bgl_named_params<P, T, R>& params,
            detail::error_property_not_found)
      {
        std::vector<default_color_type> color_vec(num_vertices(g));
        default_color_type c = white_color; // avoid warning about un-init
        depth_first_search
          (g, vis, make_iterator_property_map
           (color_vec.begin(),
            choose_const_pmap(get_param(params, vertex_index),
                              g, vertex_index), c),
           start_vertex);
      }
    };
  } // namespace detail


  template <class Visitors = null_visitor>
  class dfs_visitor {
  public:
    dfs_visitor() { }
    dfs_visitor(Visitors vis) : m_vis(vis) { }

    template <class Vertex, class Graph>
    void initialize_vertex(Vertex u, const Graph& g) {
      invoke_visitors(m_vis, u, g, ::boost::on_initialize_vertex());
    }
    template <class Vertex, class Graph>
    void start_vertex(Vertex u, const Graph& g) {
      invoke_visitors(m_vis, u, g, ::boost::on_start_vertex());
    }
    template <class Vertex, class Graph>
    void discover_vertex(Vertex u, const Graph& g) {
      invoke_visitors(m_vis, u, g, ::boost::on_discover_vertex());
    }
    template <class Edge, class Graph>
    void examine_edge(Edge u, const Graph& g) {
      invoke_visitors(m_vis, u, g, ::boost::on_examine_edge());
    }
    template <class Edge, class Graph>
    void tree_edge(Edge u, const Graph& g) {
      invoke_visitors(m_vis, u, g, ::boost::on_tree_edge());
    }
    template <class Edge, class Graph>
    void back_edge(Edge u, const Graph& g) {
      invoke_visitors(m_vis, u, g, ::boost::on_back_edge());
    }
    template <class Edge, class Graph>
    void forward_or_cross_edge(Edge u, const Graph& g) {
      invoke_visitors(m_vis, u, g, ::boost::on_forward_or_cross_edge());
    }
    template <class Vertex, class Graph>
    void finish_vertex(Vertex u, const Graph& g) {
      invoke_visitors(m_vis, u, g, ::boost::on_finish_vertex());
    }

    BOOST_GRAPH_EVENT_STUB(on_initialize_vertex,dfs)
    BOOST_GRAPH_EVENT_STUB(on_start_vertex,dfs)
    BOOST_GRAPH_EVENT_STUB(on_discover_vertex,dfs)
    BOOST_GRAPH_EVENT_STUB(on_examine_edge,dfs)
    BOOST_GRAPH_EVENT_STUB(on_tree_edge,dfs)
    BOOST_GRAPH_EVENT_STUB(on_back_edge,dfs)
    BOOST_GRAPH_EVENT_STUB(on_forward_or_cross_edge,dfs)
    BOOST_GRAPH_EVENT_STUB(on_finish_vertex,dfs)

  protected:
    Visitors m_vis;
  };
  template <class Visitors>
  dfs_visitor<Visitors>
  make_dfs_visitor(Visitors vis) {
    return dfs_visitor<Visitors>(vis);
  }
  typedef dfs_visitor<> default_dfs_visitor;


  // Named Parameter Variant
  template <class VertexListGraph, class P, class T, class R>
  void
  depth_first_search(const VertexListGraph& g,
                     const bgl_named_params<P, T, R>& params)
  {
    typedef typename property_value< bgl_named_params<P, T, R>,
      vertex_color_t>::type C;
    if (vertices(g).first == vertices(g).second)
      return;
    detail::dfs_dispatch<C>::apply
      (g,
       choose_param(get_param(params, graph_visitor),
                    make_dfs_visitor(null_visitor())),
       choose_param(get_param(params, root_vertex_t()),
                    *vertices(g).first),
       params,
       get_param(params, vertex_color)
       );
  }

  template <class IncidenceGraph, class DFSVisitor, class ColorMap>
  void depth_first_visit
    (const IncidenceGraph& g,
     typename graph_traits<IncidenceGraph>::vertex_descriptor u,
     DFSVisitor vis, ColorMap color)
  {
    vis.start_vertex(u, g);
    detail::depth_first_visit_impl(g, u, vis, color, detail::nontruth2());
  }

  template <class IncidenceGraph, class DFSVisitor, class ColorMap,
            class TerminatorFunc>
  void depth_first_visit
    (const IncidenceGraph& g,
     typename graph_traits<IncidenceGraph>::vertex_descriptor u,
     DFSVisitor vis, ColorMap color, TerminatorFunc func = TerminatorFunc())
  {
    vis.start_vertex(u, g);
    detail::depth_first_visit_impl(g, u, vis, color, func);
  }


} // namespace boost


#endif
