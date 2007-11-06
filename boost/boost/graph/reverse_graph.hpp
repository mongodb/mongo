//  (C) Copyright David Abrahams 2000.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef REVERSE_GRAPH_DWA092300_H_
# define REVERSE_GRAPH_DWA092300_H_

#include <boost/graph/adjacency_iterator.hpp>
#include <boost/graph/properties.hpp>
#include <boost/tuple/tuple.hpp>

namespace boost {

struct reverse_graph_tag { };

  namespace detail {

    template <bool isEdgeList> struct choose_rev_edge_iter { };
    template <> struct choose_rev_edge_iter<true> {
      template <class G> struct bind_ {
        typedef typename graph_traits<G>::edge_iterator type;
      };
    };
    template <> struct choose_rev_edge_iter<false> {
      template <class G> struct bind_ {
        typedef void type;
      };
    };

  } // namespace detail

template <class BidirectionalGraph, class GraphRef = const BidirectionalGraph&>
class reverse_graph {
    typedef reverse_graph<BidirectionalGraph, GraphRef> Self;
    typedef graph_traits<BidirectionalGraph> Traits;
 public:
    typedef BidirectionalGraph base_type;

    // Constructor
    reverse_graph(GraphRef g) : m_g(g) {}

    // Graph requirements
    typedef typename Traits::vertex_descriptor vertex_descriptor;
    typedef typename Traits::edge_descriptor edge_descriptor;
    typedef typename Traits::directed_category directed_category;
    typedef typename Traits::edge_parallel_category edge_parallel_category;
    typedef typename Traits::traversal_category traversal_category;

    // IncidenceGraph requirements
    typedef typename Traits::in_edge_iterator out_edge_iterator;
    typedef typename Traits::degree_size_type degree_size_type;

    // BidirectionalGraph requirements
    typedef typename Traits::out_edge_iterator in_edge_iterator;

    // AdjacencyGraph requirements
  typedef typename adjacency_iterator_generator<Self,
    vertex_descriptor, out_edge_iterator>::type adjacency_iterator;

    // VertexListGraph requirements
    typedef typename Traits::vertex_iterator vertex_iterator;

    // EdgeListGraph requirements
    enum { is_edge_list = is_convertible<traversal_category,
           edge_list_graph_tag>::value };
    typedef detail::choose_rev_edge_iter<is_edge_list> ChooseEdgeIter;
    typedef typename ChooseEdgeIter::
      template bind_<BidirectionalGraph>::type   edge_iterator;
    typedef typename Traits::vertices_size_type vertices_size_type;
    typedef typename Traits::edges_size_type edges_size_type;

    // More typedefs used by detail::edge_property_map, vertex_property_map
    typedef typename BidirectionalGraph::edge_property_type
      edge_property_type;
    typedef typename BidirectionalGraph::vertex_property_type
      vertex_property_type;
    typedef reverse_graph_tag graph_tag;

#ifndef BOOST_GRAPH_NO_BUNDLED_PROPERTIES
    // Bundled properties support
    template<typename Descriptor>
    typename graph::detail::bundled_result<BidirectionalGraph, 
                                           Descriptor>::type&
    operator[](Descriptor x)
    { return m_g[x]; }

    template<typename Descriptor>
    typename graph::detail::bundled_result<BidirectionalGraph, 
                                           Descriptor>::type const&
    operator[](Descriptor x) const
    { return m_g[x]; }
#endif // BOOST_GRAPH_NO_BUNDLED_PROPERTIES

    static vertex_descriptor null_vertex()
    { return Traits::null_vertex(); }

    // would be private, but template friends aren't portable enough.
 // private:
    GraphRef m_g;
};

#ifndef BOOST_GRAPH_NO_BUNDLED_PROPERTIES
  template<typename Graph, typename GraphRef>
  struct vertex_bundle_type<reverse_graph<Graph, GraphRef> > 
    : vertex_bundle_type<Graph> { };

  template<typename Graph, typename GraphRef>
  struct edge_bundle_type<reverse_graph<Graph, GraphRef> > 
    : edge_bundle_type<Graph> { };
#endif // BOOST_GRAPH_NO_BUNDLED_PROPERTIES

template <class BidirectionalGraph>
inline reverse_graph<BidirectionalGraph>
make_reverse_graph(const BidirectionalGraph& g)
{
    return reverse_graph<BidirectionalGraph>(g);
}

template <class BidirectionalGraph>
inline reverse_graph<BidirectionalGraph, BidirectionalGraph&>
make_reverse_graph(BidirectionalGraph& g)
{
    return reverse_graph<BidirectionalGraph, BidirectionalGraph&>(g);
}

template <class BidirectionalGraph, class GRef>
std::pair<typename reverse_graph<BidirectionalGraph>::vertex_iterator,
          typename reverse_graph<BidirectionalGraph>::vertex_iterator>
vertices(const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return vertices(g.m_g);
}

template <class BidirectionalGraph, class GRef>
std::pair<typename reverse_graph<BidirectionalGraph>::edge_iterator,
          typename reverse_graph<BidirectionalGraph>::edge_iterator>
edges(const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return edges(g.m_g);
}

template <class BidirectionalGraph, class GRef>
inline std::pair<typename BidirectionalGraph::in_edge_iterator,
                 typename BidirectionalGraph::in_edge_iterator>
out_edges(const typename BidirectionalGraph::vertex_descriptor u,
          const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return in_edges(u, g.m_g);
}

template <class BidirectionalGraph, class GRef>
inline typename BidirectionalGraph::vertices_size_type
num_vertices(const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return num_vertices(g.m_g);
}

template <class BidirectionalGraph, class GRef>
inline typename reverse_graph<BidirectionalGraph>::edges_size_type
num_edges(const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return num_edges(g.m_g);
}

template <class BidirectionalGraph, class GRef>
inline typename BidirectionalGraph::degree_size_type
out_degree(const typename BidirectionalGraph::vertex_descriptor u,
           const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return in_degree(u, g.m_g);
}

template <class BidirectionalGraph, class GRef>
inline std::pair<typename BidirectionalGraph::edge_descriptor, bool>
edge(const typename BidirectionalGraph::vertex_descriptor u,
     const typename BidirectionalGraph::vertex_descriptor v,
     const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return edge(v, u, g.m_g);
}

template <class BidirectionalGraph, class GRef>
inline std::pair<typename BidirectionalGraph::out_edge_iterator,
    typename BidirectionalGraph::out_edge_iterator>
in_edges(const typename BidirectionalGraph::vertex_descriptor u,
         const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return out_edges(u, g.m_g);
}

template <class BidirectionalGraph, class GRef>
inline std::pair<typename reverse_graph<BidirectionalGraph,GRef>::adjacency_iterator,
    typename reverse_graph<BidirectionalGraph,GRef>::adjacency_iterator>
adjacent_vertices(const typename BidirectionalGraph::vertex_descriptor u,
                  const reverse_graph<BidirectionalGraph,GRef>& g)
{
    typedef reverse_graph<BidirectionalGraph,GRef> Graph;
    typename Graph::out_edge_iterator first, last;
    tie(first, last) = out_edges(u, g);
    typedef typename Graph::adjacency_iterator adjacency_iterator;
    return std::make_pair(adjacency_iterator(first, const_cast<Graph*>(&g)),
                          adjacency_iterator(last, const_cast<Graph*>(&g)));
}

template <class BidirectionalGraph, class GRef>
inline typename BidirectionalGraph::degree_size_type
in_degree(const typename BidirectionalGraph::vertex_descriptor u,
          const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return out_degree(u, g.m_g);
}

template <class Edge, class BidirectionalGraph, class GRef>
inline typename graph_traits<BidirectionalGraph>::vertex_descriptor
source(const Edge& e, const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return target(e, g.m_g);
}

template <class Edge, class BidirectionalGraph, class GRef>
inline typename graph_traits<BidirectionalGraph>::vertex_descriptor
target(const Edge& e, const reverse_graph<BidirectionalGraph,GRef>& g)
{
    return source(e, g.m_g);
}


namespace detail {

  struct reverse_graph_vertex_property_selector {
    template <class ReverseGraph, class Property, class Tag>
    struct bind_ {
      typedef typename ReverseGraph::base_type Graph;
      typedef property_map<Graph, Tag> PMap;
      typedef typename PMap::type type;
      typedef typename PMap::const_type const_type;
    };
  };

  struct reverse_graph_edge_property_selector {
    template <class ReverseGraph, class Property, class Tag>
    struct bind_ {
      typedef typename ReverseGraph::base_type Graph;
      typedef property_map<Graph, Tag> PMap;
      typedef typename PMap::type type;
      typedef typename PMap::const_type const_type;
    };
  };

} // namespace detail

template <>
struct vertex_property_selector<reverse_graph_tag> {
  typedef detail::reverse_graph_vertex_property_selector type;
};

template <>
struct edge_property_selector<reverse_graph_tag> {
  typedef detail::reverse_graph_edge_property_selector type;
};

template <class BidirGraph, class GRef, class Property>
typename property_map<BidirGraph, Property>::type
get(Property p, reverse_graph<BidirGraph,GRef>& g)
{
  return get(p, g.m_g);
}

template <class BidirGraph, class GRef, class Property>
typename property_map<BidirGraph, Property>::const_type
get(Property p, const reverse_graph<BidirGraph,GRef>& g)
{
  const BidirGraph& gref = g.m_g; // in case GRef is non-const
  return get(p, gref);
}

template <class BidirectionalGraph, class GRef, class Property, class Key>
typename property_traits<
  typename property_map<BidirectionalGraph, Property>::const_type
>::value_type
get(Property p, const reverse_graph<BidirectionalGraph,GRef>& g, const Key& k)
{
  return get(p, g.m_g, k);
}

template <class BidirectionalGraph, class GRef, class Property, class Key, class Value>
void
put(Property p, const reverse_graph<BidirectionalGraph,GRef>& g, const Key& k,
    const Value& val)
{
  put(p, g.m_g, k, val);
}

template<typename BidirectionalGraph, typename GRef, typename Tag,
         typename Value>
inline void
set_property(const reverse_graph<BidirectionalGraph,GRef>& g, Tag tag, 
             const Value& value)
{
  set_property(g.m_g, tag, value);
}

template<typename BidirectionalGraph, typename GRef, typename Tag>
inline
typename graph_property<BidirectionalGraph, Tag>::type
get_property(const reverse_graph<BidirectionalGraph,GRef>& g, Tag tag)
{
  return get_property(g.m_g, tag);
}

} // namespace boost

#endif
