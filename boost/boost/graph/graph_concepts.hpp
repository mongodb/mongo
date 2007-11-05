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
#ifndef BOOST_GRAPH_CONCEPTS_HPP
#define BOOST_GRAPH_CONCEPTS_HPP

#include <boost/config.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/property_map.hpp>
#include <boost/graph/properties.hpp>
#include <boost/concept_check.hpp>
#include <boost/detail/workaround.hpp>

namespace boost {

  template <class T>
  struct MultiPassInputIteratorConcept {
    void constraints() {
      function_requires< InputIteratorConcept<T> >();
    }
  };

  template <class G>
  struct GraphConcept
  {
    typedef typename graph_traits<G>::vertex_descriptor vertex_descriptor;
    typedef typename graph_traits<G>::directed_category directed_category;
    typedef typename graph_traits<G>::edge_parallel_category
      edge_parallel_category;
    typedef typename graph_traits<G>::traversal_category
      traversal_category;
    void constraints() {
      function_requires< DefaultConstructibleConcept<vertex_descriptor> >();
      function_requires< EqualityComparableConcept<vertex_descriptor> >();
      function_requires< AssignableConcept<vertex_descriptor> >();
    }
    G g;
  };

  template <class G>
  struct IncidenceGraphConcept
  {
    typedef typename graph_traits<G>::edge_descriptor edge_descriptor;
    typedef typename graph_traits<G>::out_edge_iterator
      out_edge_iterator;
    typedef typename graph_traits<G>::traversal_category
      traversal_category;
    void constraints() {
      function_requires< GraphConcept<G> >();
      function_requires< MultiPassInputIteratorConcept<out_edge_iterator> >();
      function_requires< DefaultConstructibleConcept<edge_descriptor> >();
      function_requires< EqualityComparableConcept<edge_descriptor> >();
      function_requires< AssignableConcept<edge_descriptor> >();
      function_requires< ConvertibleConcept<traversal_category,
        incidence_graph_tag> >();

      p = out_edges(u, g);
      n = out_degree(u, g);
      e = *p.first;
      u = source(e, g);
      v = target(e, g);
      const_constraints(g);
    }
    void const_constraints(const G& cg) {
      p = out_edges(u, cg);
      n = out_degree(u, cg);
      e = *p.first;
      u = source(e, cg);
      v = target(e, cg);
    }
    std::pair<out_edge_iterator, out_edge_iterator> p;
    typename graph_traits<G>::vertex_descriptor u, v;
    typename graph_traits<G>::edge_descriptor e;
    typename graph_traits<G>::degree_size_type n;
    G g;
  };

  template <class G>
  struct BidirectionalGraphConcept
  {
    typedef typename graph_traits<G>::in_edge_iterator
      in_edge_iterator;
    typedef typename graph_traits<G>::traversal_category
      traversal_category;
    void constraints() {
      function_requires< IncidenceGraphConcept<G> >();
      function_requires< MultiPassInputIteratorConcept<in_edge_iterator> >();
      function_requires< ConvertibleConcept<traversal_category,
        bidirectional_graph_tag> >();

      p = in_edges(v, g);
      n = in_degree(v, g);
      e = *p.first;
      const_constraints(g);
    }
    void const_constraints(const G& cg) {
      p = in_edges(v, cg);
      n = in_degree(v, cg);
      e = *p.first;
    }
    std::pair<in_edge_iterator, in_edge_iterator> p;
    typename graph_traits<G>::vertex_descriptor v;
    typename graph_traits<G>::edge_descriptor e;
    typename graph_traits<G>::degree_size_type n;
    G g;
  };

  template <class G>
  struct AdjacencyGraphConcept
  {
    typedef typename graph_traits<G>::adjacency_iterator
      adjacency_iterator;
    typedef typename graph_traits<G>::traversal_category
      traversal_category;
    void constraints() {
      function_requires< GraphConcept<G> >();
      function_requires< MultiPassInputIteratorConcept<adjacency_iterator> >();
      function_requires< ConvertibleConcept<traversal_category,
        adjacency_graph_tag> >();

      p = adjacent_vertices(v, g);
      v = *p.first;
      const_constraints(g);
    }
    void const_constraints(const G& cg) {
      p = adjacent_vertices(v, cg);
    }
    std::pair<adjacency_iterator,adjacency_iterator> p;
    typename graph_traits<G>::vertex_descriptor v;
    G g;
  };

// dwa 2003/7/11 -- This clearly shouldn't be necessary, but if
// you want to use vector_as_graph, it is!  I'm sure the graph
// library leaves these out all over the place.  Probably a
// redesign involving specializing a template with a static
// member function is in order :(
//
// It is needed in order to allow us to write using boost::vertices as
// needed for ADL when using vector_as_graph below.
#if !defined(BOOST_NO_ARGUMENT_DEPENDENT_LOOKUP)            \
 && !BOOST_WORKAROUND(__GNUC__, <= 2)                       \
 && !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
# define BOOST_VECTOR_AS_GRAPH_GRAPH_ADL_HACK
#endif 

#ifdef BOOST_VECTOR_AS_GRAPH_GRAPH_ADL_HACK
template <class T>
typename T::ThereReallyIsNoMemberByThisNameInT vertices(T const&);
#endif      

  template <class G>
  struct VertexListGraphConcept
  {
    typedef typename graph_traits<G>::vertex_iterator vertex_iterator;
    typedef typename graph_traits<G>::vertices_size_type vertices_size_type;
    typedef typename graph_traits<G>::traversal_category
      traversal_category;
    void constraints() {
      function_requires< GraphConcept<G> >();
      function_requires< MultiPassInputIteratorConcept<vertex_iterator> >();
      function_requires< ConvertibleConcept<traversal_category,
        vertex_list_graph_tag> >();

#ifdef BOOST_VECTOR_AS_GRAPH_GRAPH_ADL_HACK
      // dwa 2003/7/11 -- This clearly shouldn't be necessary, but if
      // you want to use vector_as_graph, it is!  I'm sure the graph
      // library leaves these out all over the place.  Probably a
      // redesign involving specializing a template with a static
      // member function is in order :(
      using boost::vertices;
#endif      
      p = vertices(g);
      v = *p.first;
      const_constraints(g);
    }
    void const_constraints(const G& cg) {
#ifdef BOOST_VECTOR_AS_GRAPH_GRAPH_ADL_HACK
      // dwa 2003/7/11 -- This clearly shouldn't be necessary, but if
      // you want to use vector_as_graph, it is!  I'm sure the graph
      // library leaves these out all over the place.  Probably a
      // redesign involving specializing a template with a static
      // member function is in order :(
      using boost::vertices;
#endif 
      
      p = vertices(cg);
      v = *p.first;
      V = num_vertices(cg);
    }
    std::pair<vertex_iterator,vertex_iterator> p;
    typename graph_traits<G>::vertex_descriptor v;
    G g;
    vertices_size_type V;
  };

  template <class G>
  struct EdgeListGraphConcept
  {
    typedef typename graph_traits<G>::edge_descriptor edge_descriptor;
    typedef typename graph_traits<G>::edge_iterator edge_iterator;
    typedef typename graph_traits<G>::edges_size_type edges_size_type;
    typedef typename graph_traits<G>::traversal_category
      traversal_category;
    void constraints() {
      function_requires< GraphConcept<G> >();
      function_requires< MultiPassInputIteratorConcept<edge_iterator> >();
      function_requires< DefaultConstructibleConcept<edge_descriptor> >();
      function_requires< EqualityComparableConcept<edge_descriptor> >();
      function_requires< AssignableConcept<edge_descriptor> >();
      function_requires< ConvertibleConcept<traversal_category,
        edge_list_graph_tag> >();

      p = edges(g);
      e = *p.first;
      u = source(e, g);
      v = target(e, g);
      const_constraints(g);
    }
    void const_constraints(const G& cg) {
      p = edges(cg);
      E = num_edges(cg);
      e = *p.first;
      u = source(e, cg);
      v = target(e, cg);
    }
    std::pair<edge_iterator,edge_iterator> p;
    typename graph_traits<G>::vertex_descriptor u, v;
    typename graph_traits<G>::edge_descriptor e;
    edges_size_type E;
    G g;
  };

  template <class G>
  struct VertexAndEdgeListGraphConcept
  {
    void constraints() {
      function_requires< VertexListGraphConcept<G> >();    
      function_requires< EdgeListGraphConcept<G> >();
    }
  };

  // Where to put the requirement for this constructor?
  //      G g(n_vertices);
  // Not in mutable graph, then LEDA graph's can't be models of
  // MutableGraph.

  template <class G>
  struct EdgeMutableGraphConcept
  {
    typedef typename graph_traits<G>::edge_descriptor edge_descriptor;
    void constraints() {
      p = add_edge(u, v, g);
      remove_edge(u, v, g);
      remove_edge(e, g);
      clear_vertex(v, g);
    }
    G g;
    edge_descriptor e;
    std::pair<edge_descriptor, bool> p;
    typename graph_traits<G>::vertex_descriptor u, v;
  };

  template <class G>
  struct VertexMutableGraphConcept
  {
    void constraints() {
      v = add_vertex(g);
      remove_vertex(v, g);
    }
    G g;
    typename graph_traits<G>::vertex_descriptor u, v;
  };

  template <class G>
  struct MutableGraphConcept
  {
    void constraints() {
      function_requires< EdgeMutableGraphConcept<G> >();
      function_requires< VertexMutableGraphConcept<G> >();
    }
  };

  template <class edge_descriptor>
  struct dummy_edge_predicate {
    bool operator()(const edge_descriptor&) const {
      return false;
    }
  };

  template <class G>
  struct MutableIncidenceGraphConcept
  {
    void constraints() {
      function_requires< MutableGraphConcept<G> >();
      remove_edge(iter, g);
      remove_out_edge_if(u, p, g);
    }
    G g;
    typedef typename graph_traits<G>::edge_descriptor edge_descriptor;
    dummy_edge_predicate<edge_descriptor> p;
    typename boost::graph_traits<G>::vertex_descriptor u;
    typename boost::graph_traits<G>::out_edge_iterator iter;
  };

  template <class G>
  struct MutableBidirectionalGraphConcept
  {
    void constraints() {
      function_requires< MutableIncidenceGraphConcept<G> >();
      remove_in_edge_if(u, p, g);
    }
    G g;
    typedef typename graph_traits<G>::edge_descriptor edge_descriptor;
    dummy_edge_predicate<edge_descriptor> p;
    typename boost::graph_traits<G>::vertex_descriptor u;
  };

  template <class G>
  struct MutableEdgeListGraphConcept
  {
    void constraints() {
      function_requires< EdgeMutableGraphConcept<G> >();
      remove_edge_if(p, g);
    }
    G g;
    typedef typename graph_traits<G>::edge_descriptor edge_descriptor;
    dummy_edge_predicate<edge_descriptor> p;
  };

  template <class G>
  struct VertexMutablePropertyGraphConcept
  {
    void constraints() {
      function_requires< VertexMutableGraphConcept<G> >();
      v = add_vertex(vp, g);
    }
    G g;
    typename graph_traits<G>::vertex_descriptor v;
    typename vertex_property<G>::type vp;
  };

  template <class G>
  struct EdgeMutablePropertyGraphConcept
  {
    typedef typename graph_traits<G>::edge_descriptor edge_descriptor;
    void constraints() {
      function_requires< EdgeMutableGraphConcept<G> >();
      p = add_edge(u, v, ep, g);
    }
    G g;
    std::pair<edge_descriptor, bool> p;
    typename graph_traits<G>::vertex_descriptor u, v;
    typename edge_property<G>::type ep;
  };

  template <class G>
  struct AdjacencyMatrixConcept
  {
    typedef typename graph_traits<G>::edge_descriptor edge_descriptor;
    void constraints() {
      function_requires< GraphConcept<G> >();
      
      p = edge(u, v, g);
      const_constraints(g);
    }
    void const_constraints(const G& cg) {
      p = edge(u, v, cg);
    }
    typename graph_traits<G>::vertex_descriptor u, v;
    std::pair<edge_descriptor, bool> p;
    G g;
  };

  template <class G, class X, class Property>
  struct ReadablePropertyGraphConcept
  {
    typedef typename property_map<G, Property>::const_type const_Map;
    void constraints() {
      function_requires< GraphConcept<G> >();
      function_requires< ReadablePropertyMapConcept<const_Map, X> >();

      const_constraints(g);
    }
    void const_constraints(const G& cg) {
      const_Map pmap = get(Property(), cg);
      pval = get(Property(), cg, x);
      ignore_unused_variable_warning(pmap);
    }
    G g;
    X x;
    typename property_traits<const_Map>::value_type pval;
  };

  template <class G, class X, class Property>
  struct PropertyGraphConcept
  {
    typedef typename property_map<G, Property>::type Map;
    void constraints() {
      function_requires< ReadablePropertyGraphConcept<G, X, Property> >();
      function_requires< ReadWritePropertyMapConcept<Map, X> >();

      Map pmap = get(Property(), g);
      pval = get(Property(), g, x);
      put(Property(), g, x, pval);
      ignore_unused_variable_warning(pmap);
    }
    G g;
    X x;
    typename property_traits<Map>::value_type pval;
  };

  template <class G, class X, class Property>
  struct LvaluePropertyGraphConcept
  {
    typedef typename property_map<G, Property>::type Map;
    typedef typename property_map<G, Property>::const_type const_Map;
    void constraints() {
      function_requires< ReadablePropertyGraphConcept<G, X, Property> >();
      function_requires< LvaluePropertyMapConcept<const_Map, X> >();

      pval = get(Property(), g, x);
      put(Property(), g, x, pval);
    }
    G g;
    X x;
    typename property_traits<Map>::value_type pval;
  };

  // This needs to move out of the graph library
  template <class B>
  struct BufferConcept
  {
    void constraints() {
      b.push(t);
      b.pop();
      typename B::value_type& v = b.top();
      const_constraints(b);
      ignore_unused_variable_warning(v);
    }
    void const_constraints(const B& cb) {
      const typename B::value_type& v = cb.top();
      n = cb.size();
      bool e = cb.empty();
      ignore_unused_variable_warning(v);
      ignore_unused_variable_warning(e);
    }
    typename B::size_type n;
    typename B::value_type t;
    B b;
  };

  template <class C>
  struct ColorValueConcept
  {
    void constraints() {
      function_requires< EqualityComparableConcept<C> >();
      function_requires< DefaultConstructibleConcept<C> >();

      c = color_traits<C>::white();
      c = color_traits<C>::gray();
      c = color_traits<C>::black();
    }
    C c;
  };

  template <class M, class I, class V>
  struct BasicMatrixConcept
  {
    void constraints() {
      V& elt = A[i][j];
      const_constraints(A);
      ignore_unused_variable_warning(elt);      
    }
    void const_constraints(const M& cA) {
      const V& elt = cA[i][j];
      ignore_unused_variable_warning(elt);      
    }
    M A;
    I i, j;
  };

} // namespace boost

#endif /* BOOST_GRAPH_CONCEPTS_H */
