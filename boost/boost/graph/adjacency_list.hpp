//=======================================================================
// Copyright 1997, 1998, 1999, 2000 University of Notre Dame.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#ifndef BOOST_GRAPH_ADJACENCY_LIST_HPP
#define BOOST_GRAPH_ADJACENCY_LIST_HPP


#include <boost/config.hpp>

#include <vector>
#include <list>
#include <set>

#if !defined BOOST_NO_HASH
#  ifdef BOOST_HASH_SET_HEADER
#    include BOOST_HASH_SET_HEADER
#  else
#    include <hash_set>
#  endif
#endif

#if !defined BOOST_NO_SLIST
#  ifdef BOOST_SLIST_HEADER
#    include BOOST_SLIST_HEADER
#  else
#    include <slist>
#  endif
#endif

#include <boost/graph/graph_traits.hpp>
#include <boost/graph/graph_selectors.hpp>
#include <boost/property_map.hpp>
#include <boost/pending/ct_if.hpp>
#include <boost/graph/detail/edge.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/graph/properties.hpp>

namespace boost {

  //===========================================================================
  // Selectors for the VertexList and EdgeList template parameters of
  // adjacency_list, and the container_gen traits class which is used
  // to map the selectors to the container type used to implement the
  // graph.
  //
  // The main container_gen traits class uses partial specialization,
  // so we also include a workaround.

#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

#if !defined BOOST_NO_SLIST
  struct slistS {};  
#endif

  struct vecS  { };
  struct listS { };
  struct setS { };
  struct multisetS { };
  struct mapS  { };
#if !defined BOOST_NO_HASH
  struct hash_mapS { };
  struct hash_setS { };
#endif

  template <class Selector, class ValueType>
  struct container_gen { };

  template <class ValueType>
  struct container_gen<listS, ValueType> {
    typedef std::list<ValueType> type;
  };
#if !defined BOOST_NO_SLIST
  template <class ValueType>
  struct container_gen<slistS, ValueType> {
    typedef BOOST_STD_EXTENSION_NAMESPACE::slist<ValueType> type;
  };
#endif
  template <class ValueType>
  struct container_gen<vecS, ValueType> {
    typedef std::vector<ValueType> type;
  };

  template <class ValueType>
  struct container_gen<mapS, ValueType> {
    typedef std::set<ValueType> type;
  };

  template <class ValueType>
  struct container_gen<setS, ValueType> {
    typedef std::set<ValueType> type;
  };

  template <class ValueType>
  struct container_gen<multisetS, ValueType> {
    typedef std::multiset<ValueType> type;
  };

#if !defined BOOST_NO_HASH
  template <class ValueType>
  struct container_gen<hash_mapS, ValueType> {
    typedef BOOST_STD_EXTENSION_NAMESPACE::hash_set<ValueType> type;
  };

  template <class ValueType>
  struct container_gen<hash_setS, ValueType> {
    typedef BOOST_STD_EXTENSION_NAMESPACE::hash_set<ValueType> type;
  };
#endif

#else // !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

#if !defined BOOST_NO_SLIST
  struct slistS {
    template <class T>
    struct bind_ { typedef BOOST_STD_EXTENSION_NAMESPACE::slist<T> type; };
  };
#endif

  struct vecS  {
    template <class T>
    struct bind_ { typedef std::vector<T> type; };
  };

  struct listS { 
    template <class T>
    struct bind_ { typedef std::list<T> type; };
  };

  struct setS  { 
    template <class T>
    struct bind_ { typedef std::set<T, std::less<T> > type; };
  };

  struct multisetS  { 
    template <class T>
    struct bind_ { typedef std::multiset<T, std::less<T> > type; };
  };

#if !defined BOOST_NO_HASH
  struct hash_setS { 
    template <class T>
    struct bind_ { typedef BOOST_STD_EXTENSION_NAMESPACE::hash_set<T, std::less<T> > type; };
  };
#endif

  struct mapS  { 
    template <class T>
    struct bind_ { typedef std::set<T, std::less<T> > type; };
  };

#if !defined BOOST_NO_HASH
  struct hash_mapS { 
    template <class T>
    struct bind_ { typedef BOOST_STD_EXTENSION_NAMESPACE::hash_set<T, std::less<T> > type; };
  };
#endif

  template <class Selector> struct container_selector {
    typedef vecS type;
  };

#define BOOST_CONTAINER_SELECTOR(NAME) \
  template <> struct container_selector<NAME>  { \
    typedef NAME type; \
  }

  BOOST_CONTAINER_SELECTOR(vecS);
  BOOST_CONTAINER_SELECTOR(listS);
  BOOST_CONTAINER_SELECTOR(mapS);
  BOOST_CONTAINER_SELECTOR(setS);
  BOOST_CONTAINER_SELECTOR(multisetS);
#if !defined BOOST_NO_HASH
  BOOST_CONTAINER_SELECTOR(hash_mapS);
#endif
#if !defined BOOST_NO_SLIST
  BOOST_CONTAINER_SELECTOR(slistS);
#endif

  template <class Selector, class ValueType>
  struct container_gen {
    typedef typename container_selector<Selector>::type Select;
    typedef typename Select:: template bind_<ValueType>::type type;
  };

#endif // !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

  template <class StorageSelector>
  struct parallel_edge_traits { };

  template <>
  struct parallel_edge_traits<vecS> { 
    typedef allow_parallel_edge_tag type; };

  template <>
  struct parallel_edge_traits<listS> { 
    typedef allow_parallel_edge_tag type; };

#if !defined BOOST_NO_SLIST
  template <>
  struct parallel_edge_traits<slistS> { 
    typedef allow_parallel_edge_tag type; };
#endif

  template <>
  struct parallel_edge_traits<setS> { 
    typedef disallow_parallel_edge_tag type; };

  template <>
  struct parallel_edge_traits<multisetS> { 
    typedef allow_parallel_edge_tag type; };

#if !defined BOOST_NO_HASH
  template <>
  struct parallel_edge_traits<hash_setS> {
    typedef disallow_parallel_edge_tag type; 
  };
#endif

  // mapS is obsolete, replaced with setS
  template <>
  struct parallel_edge_traits<mapS> { 
    typedef disallow_parallel_edge_tag type; };

#if !defined BOOST_NO_HASH
  template <>
  struct parallel_edge_traits<hash_mapS> {
    typedef disallow_parallel_edge_tag type; 
  };
#endif

  namespace detail {
    template <class Directed> struct is_random_access { 
      enum { value = false};
      typedef false_type type;
    };
    template <>
    struct is_random_access<vecS> { 
      enum { value = true }; 
      typedef true_type type;
    };

  } // namespace detail



  //===========================================================================
  // The adjacency_list_traits class, which provides a way to access
  // some of the associated types of an adjacency_list type without
  // having to first create the adjacency_list type. This is useful
  // when trying to create interior vertex or edge properties who's
  // value type is a vertex or edge descriptor.

  template <class OutEdgeListS = vecS,
            class VertexListS = vecS,
            class DirectedS = directedS>
  struct adjacency_list_traits
  {
    typedef typename detail::is_random_access<VertexListS>::type
      is_rand_access;
    typedef typename DirectedS::is_bidir_t is_bidir;
    typedef typename DirectedS::is_directed_t is_directed;

    typedef typename boost::ct_if_t<is_bidir,
      bidirectional_tag,
      typename boost::ct_if_t<is_directed,
        directed_tag, undirected_tag
      >::type
    >::type directed_category;

    typedef typename parallel_edge_traits<OutEdgeListS>::type
      edge_parallel_category;

    typedef void* vertex_ptr;
    typedef typename boost::ct_if_t<is_rand_access,
      std::size_t, vertex_ptr>::type vertex_descriptor;
    typedef detail::edge_desc_impl<directed_category, vertex_descriptor>
      edge_descriptor;
  };

} // namespace boost

#include <boost/graph/detail/adjacency_list.hpp>

namespace boost {

  //===========================================================================
  // The adjacency_list class.
  //

  template <class OutEdgeListS = vecS, // a Sequence or an AssociativeContainer
            class VertexListS = vecS, // a Sequence or a RandomAccessContainer
            class DirectedS = directedS,
            class VertexProperty = no_property,
            class EdgeProperty = no_property,
            class GraphProperty = no_property,
            class EdgeListS = listS>
  class adjacency_list
    : public detail::adj_list_gen<
      adjacency_list<OutEdgeListS,VertexListS,DirectedS,
                     VertexProperty,EdgeProperty,GraphProperty,EdgeListS>,
      VertexListS, OutEdgeListS, DirectedS, 
#if !defined(BOOST_GRAPH_NO_BUNDLED_PROPERTIES)
      typename detail::retag_property_list<vertex_bundle_t,
                                           VertexProperty>::type,
      typename detail::retag_property_list<edge_bundle_t, EdgeProperty>::type,
#else
      VertexProperty, EdgeProperty,
#endif
      GraphProperty, EdgeListS>::type
  {
#if !defined(BOOST_GRAPH_NO_BUNDLED_PROPERTIES)
    typedef typename detail::retag_property_list<vertex_bundle_t,
                                                 VertexProperty>::retagged
      maybe_vertex_bundled;

     typedef typename detail::retag_property_list<edge_bundle_t,
                                                  EdgeProperty>::retagged
      maybe_edge_bundled;
#endif

  public:
#if !defined(BOOST_GRAPH_NO_BUNDLED_PROPERTIES)
    typedef typename detail::retag_property_list<vertex_bundle_t,
                                                 VertexProperty>::type
      vertex_property_type;
    typedef typename detail::retag_property_list<edge_bundle_t,
                                                 EdgeProperty>::type
      edge_property_type;

    // The types that are actually bundled
    typedef typename ct_if<(is_same<maybe_vertex_bundled, no_property>::value),
                           no_vertex_bundle,
                           maybe_vertex_bundled>::type vertex_bundled;
    typedef typename ct_if<(is_same<maybe_edge_bundled, no_property>::value),
                           no_edge_bundle,
                           maybe_edge_bundled>::type edge_bundled;
#else
    typedef VertexProperty vertex_property_type;
    typedef EdgeProperty edge_property_type;
    typedef no_vertex_bundle vertex_bundled;
    typedef no_edge_bundle edge_bundled;
#endif

  private:
    typedef adjacency_list self;
    typedef typename detail::adj_list_gen<
      self, VertexListS, OutEdgeListS, DirectedS, 
      vertex_property_type, edge_property_type, GraphProperty, EdgeListS
    >::type Base;

  public:
    typedef typename Base::stored_vertex stored_vertex;
    typedef typename Base::vertices_size_type vertices_size_type;
    typedef typename Base::edges_size_type edges_size_type;
    typedef typename Base::degree_size_type degree_size_type;
    typedef typename Base::vertex_descriptor vertex_descriptor;
    typedef typename Base::edge_descriptor edge_descriptor;
    typedef OutEdgeListS out_edge_list_selector;
    typedef VertexListS vertex_list_selector;
    typedef DirectedS directed_selector;
    typedef EdgeListS edge_list_selector;

    typedef GraphProperty graph_property_type;

    inline adjacency_list(const GraphProperty& p = GraphProperty()) 
      : m_property(p) { }

    inline adjacency_list(const adjacency_list& x)
      : Base(x), m_property(x.m_property) { }

    inline adjacency_list& operator=(const adjacency_list& x) {
      // TBD: probably should give the strong guarantee
      if (&x != this) {
        Base::operator=(x);
        m_property = x.m_property;
      }
      return *this;
    }

    // Required by Mutable Graph
    inline adjacency_list(vertices_size_type num_vertices, 
                          const GraphProperty& p = GraphProperty())
      : Base(num_vertices), m_property(p) { }

#if !defined(BOOST_MSVC) || BOOST_MSVC >= 1300
    // Required by Iterator Constructible Graph
    template <class EdgeIterator>
    inline adjacency_list(EdgeIterator first, EdgeIterator last,
                          vertices_size_type n,
                          edges_size_type = 0,
                          const GraphProperty& p = GraphProperty())
      : Base(n, first, last), m_property(p) { }

    template <class EdgeIterator, class EdgePropertyIterator>
    inline adjacency_list(EdgeIterator first, EdgeIterator last,
                          EdgePropertyIterator ep_iter,
                          vertices_size_type n,
                          edges_size_type = 0,
                          const GraphProperty& p = GraphProperty())
      : Base(n, first, last, ep_iter), m_property(p) { }
#endif

    void swap(adjacency_list& x) {
      // Is there a more efficient way to do this?
      adjacency_list tmp(x);
      x = *this;
      *this = tmp;
    }

#ifndef BOOST_GRAPH_NO_BUNDLED_PROPERTIES
    // Directly access a vertex or edge bundle
    vertex_bundled& operator[](vertex_descriptor v)
    { return get(vertex_bundle, *this)[v]; }

    const vertex_bundled& operator[](vertex_descriptor v) const
    { return get(vertex_bundle, *this)[v]; }

    edge_bundled& operator[](edge_descriptor e)
    { return get(edge_bundle, *this)[e]; }

    const edge_bundled& operator[](edge_descriptor e) const
    { return get(edge_bundle, *this)[e]; }
#endif

    //  protected:  (would be protected if friends were more portable)
    GraphProperty m_property;
  };

  template <class OEL, class VL, class DirS, class VP,class EP, class GP,
            class EL, class Tag, class Value>
  inline void
  set_property(adjacency_list<OEL,VL,DirS,VP,EP,GP,EL>& g, Tag,
               const Value& value) {
    get_property_value(g.m_property, Tag()) = value;;
  }

  template <class OEL, class VL, class DirS, class VP, class EP, class GP,
            class Tag, class EL>
  inline
  typename graph_property<adjacency_list<OEL,VL,DirS,VP,EP,GP,EL>, Tag>::type&
  get_property(adjacency_list<OEL,VL,DirS,VP,EP,GP,EL>& g, Tag) {
    return get_property_value(g.m_property, Tag());
  }

  template <class OEL, class VL, class DirS, class VP, class EP, class GP,
            class Tag, class EL>
  inline
  const
  typename graph_property<adjacency_list<OEL,VL,DirS,VP,EP,GP,EL>, Tag>::type&
  get_property(const adjacency_list<OEL,VL,DirS,VP,EP,GP,EL>& g, Tag) {
    return get_property_value(g.m_property, Tag());
  }

  // dwa 09/25/00 - needed to be more explicit so reverse_graph would work.
  template <class Directed, class Vertex,
      class OutEdgeListS,
      class VertexListS,
      class DirectedS,
      class VertexProperty,
      class EdgeProperty,
      class GraphProperty, class EdgeListS>
  inline Vertex
  source(const detail::edge_base<Directed,Vertex>& e,
         const adjacency_list<OutEdgeListS, VertexListS, DirectedS,
                 VertexProperty, EdgeProperty, GraphProperty, EdgeListS>&)
  {
    return e.m_source;
  }

  template <class Directed, class Vertex, class OutEdgeListS,
      class VertexListS, class DirectedS, class VertexProperty,
      class EdgeProperty, class GraphProperty, class EdgeListS>
  inline Vertex
  target(const detail::edge_base<Directed,Vertex>& e,
         const adjacency_list<OutEdgeListS, VertexListS, DirectedS,
              VertexProperty, EdgeProperty, GraphProperty, EdgeListS>&)
  {
    return e.m_target;
  }

  // Support for bundled properties
#ifndef BOOST_GRAPH_NO_BUNDLED_PROPERTIES
  template<typename OutEdgeListS, typename VertexListS, typename DirectedS, typename VertexProperty,
           typename EdgeProperty, typename GraphProperty, typename EdgeListS, typename T, typename Bundle>
  inline
  typename property_map<adjacency_list<OutEdgeListS, VertexListS, DirectedS, VertexProperty, EdgeProperty,
                                       GraphProperty, EdgeListS>, T Bundle::*>::type
  get(T Bundle::* p, adjacency_list<OutEdgeListS, VertexListS, DirectedS, VertexProperty, EdgeProperty,
                                    GraphProperty, EdgeListS>& g)
  {
    typedef typename property_map<adjacency_list<OutEdgeListS, VertexListS, DirectedS, VertexProperty, 
                                                 EdgeProperty, GraphProperty, EdgeListS>, T Bundle::*>::type
      result_type;
    return result_type(&g, p);
  }
  
  template<typename OutEdgeListS, typename VertexListS, typename DirectedS, typename VertexProperty,
           typename EdgeProperty, typename GraphProperty, typename EdgeListS, typename T, typename Bundle>
  inline
  typename property_map<adjacency_list<OutEdgeListS, VertexListS, DirectedS, VertexProperty, EdgeProperty,
                                       GraphProperty, EdgeListS>, T Bundle::*>::const_type
  get(T Bundle::* p, adjacency_list<OutEdgeListS, VertexListS, DirectedS, VertexProperty, EdgeProperty,
                                    GraphProperty, EdgeListS> const & g)
  {
    typedef typename property_map<adjacency_list<OutEdgeListS, VertexListS, DirectedS, VertexProperty, 
                                                 EdgeProperty, GraphProperty, EdgeListS>, T Bundle::*>::const_type
      result_type;
    return result_type(&g, p);
  }

  template<typename OutEdgeListS, typename VertexListS, typename DirectedS, typename VertexProperty,
           typename EdgeProperty, typename GraphProperty, typename EdgeListS, typename T, typename Bundle,
           typename Key>
  inline T
  get(T Bundle::* p, adjacency_list<OutEdgeListS, VertexListS, DirectedS, VertexProperty, EdgeProperty,
                                    GraphProperty, EdgeListS> const & g, const Key& key)
  {
    return get(get(p, g), key);
  }

  template<typename OutEdgeListS, typename VertexListS, typename DirectedS, typename VertexProperty,
           typename EdgeProperty, typename GraphProperty, typename EdgeListS, typename T, typename Bundle,
           typename Key>
  inline void
  put(T Bundle::* p, adjacency_list<OutEdgeListS, VertexListS, DirectedS, VertexProperty, EdgeProperty,
                                    GraphProperty, EdgeListS>& g, const Key& key, const T& value)
  {
    put(get(p, g), key, value);
  }

#endif


} // namespace boost


#endif // BOOST_GRAPH_ADJACENCY_LIST_HPP
