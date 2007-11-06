//=======================================================================
// Copyright 1997, 1998, 1999, 2000 University of Notre Dame.
// Copyright 2004 The Trustees of Indiana University.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek, Douglas Gregor
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================
#ifndef BOOST_GRAPH_LEDA_HPP
#define BOOST_GRAPH_LEDA_HPP

#include <boost/config.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/properties.hpp>

#include <LEDA/graph.h>
#include <LEDA/node_array.h>
#include <LEDA/node_map.h>

// The functions and classes in this file allows the user to
// treat a LEDA GRAPH object as a boost graph "as is". No
// wrapper is needed for the GRAPH object.

// Remember to define LEDA_PREFIX so that LEDA types such as
// leda_edge show up as "leda_edge" and not just "edge".

// Warning: this implementation relies on partial specialization
// for the graph_traits class (so it won't compile with Visual C++)

// Warning: this implementation is in alpha and has not been tested

#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
namespace boost {

  struct leda_graph_traversal_category : 
    public virtual bidirectional_graph_tag,
    public virtual adjacency_graph_tag,
    public virtual vertex_list_graph_tag { };

  template <class vtype, class etype>
  struct graph_traits< leda::GRAPH<vtype,etype> > {
    typedef leda_node vertex_descriptor;
    typedef leda_edge edge_descriptor;

    class adjacency_iterator 
      : public iterator_facade<adjacency_iterator,
                               leda_node,
                               bidirectional_traversal_tag,
                               leda_node,
                               const leda_node*>
    {
    public:
      explicit adjacency_iterator(leda_edge edge = 0) : base(edge) {}

    private:
      leda_node dereference() const { return leda::target(base); }

      bool equal(const adjacency_iterator& other) const
      { return base == other.base; }

      void increment() { base = Succ_Adj_Edge(base, 0); }
      void decrement() { base = Pred_Adj_Edge(base, 0); }

      leda_edge base;

      friend class iterator_core_access;
    };
      
    class out_edge_iterator 
      : public iterator_facade<out_edge_iterator,
                               leda_edge,
                               bidirectional_traversal_tag,
                               const leda_edge&,
                               const leda_edge*>
    {
    public:
      explicit out_edge_iterator(leda_edge edge = 0) : base(edge) {}

    private:
      const leda_edge& dereference() const { return base; }

      bool equal(const out_edge_iterator& other) const
      { return base == other.base; }

      void increment() { base = Succ_Adj_Edge(base, 0); }
      void decrement() { base = Pred_Adj_Edge(base, 0); }

      leda_edge base;

      friend class iterator_core_access;
    };
      
    class in_edge_iterator 
      : public iterator_facade<in_edge_iterator,
                               leda_edge,
                               bidirectional_traversal_tag,
                               const leda_edge&,
                               const leda_edge*>
    {
    public:
      explicit in_edge_iterator(leda_edge edge = 0) : base(edge) {}

    private:
      const leda_edge& dereference() const { return base; }

      bool equal(const in_edge_iterator& other) const
      { return base == other.base; }

      void increment() { base = Succ_Adj_Edge(base, 1); }
      void decrement() { base = Pred_Adj_Edge(base, 1); }

      leda_edge base;

      friend class iterator_core_access;
    };

    class vertex_iterator 
      : public iterator_facade<vertex_iterator,
                               leda_node,
                               bidirectional_traversal_tag,
                               const leda_node&,
                               const leda_node*>
    {
    public:
      vertex_iterator(leda_node node = 0, 
                      const leda::GRAPH<vtype, etype>* g = 0)
        : base(node), g(g) {}

    private:
      const leda_node& dereference() const { return base; }

      bool equal(const vertex_iterator& other) const
      { return base == other.base; }

      void increment() { base = g->succ_node(base); }
      void decrement() { base = g->pred_node(base); }

      leda_node base;
      const leda::GRAPH<vtype, etype>* g;

      friend class iterator_core_access;
    };

    typedef directed_tag directed_category;
    typedef allow_parallel_edge_tag edge_parallel_category; // not sure here
    typedef leda_graph_traversal_category traversal_category;
    typedef int vertices_size_type;
    typedef int edges_size_type;
    typedef int degree_size_type;
  };

  template <class vtype, class etype>
  struct vertex_property< leda::GRAPH<vtype,etype> > {
    typedef vtype type;
  };

  template <class vtype, class etype>
  struct edge_property< leda::GRAPH<vtype,etype> > {
    typedef etype type;
  };

} // namespace boost
#endif

namespace boost {

  template <class vtype, class etype>
  typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor
  source(typename graph_traits< leda::GRAPH<vtype,etype> >::edge_descriptor e,
         const leda::GRAPH<vtype,etype>& g)
  {
    return source(e);
  }

  template <class vtype, class etype>
  typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor
  target(typename graph_traits< leda::GRAPH<vtype,etype> >::edge_descriptor e,
         const leda::GRAPH<vtype,etype>& g)
  {
    return target(e);
  }

  template <class vtype, class etype>
  inline std::pair<
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_iterator,
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_iterator >  
  vertices(const leda::GRAPH<vtype,etype>& g)
  {
    typedef typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_iterator
      Iter;
    return std::make_pair( Iter(g.first_node(),&g), Iter(0,&g) );
  }

  // no edges(g) function

  template <class vtype, class etype>
  inline std::pair<
    typename graph_traits< leda::GRAPH<vtype,etype> >::out_edge_iterator,
    typename graph_traits< leda::GRAPH<vtype,etype> >::out_edge_iterator >  
  out_edges(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u, 
    const leda::GRAPH<vtype,etype>& g)
  {
    typedef typename graph_traits< leda::GRAPH<vtype,etype> >
      ::out_edge_iterator Iter;
    return std::make_pair( Iter(First_Adj_Edge(u,0)), Iter(0) );
  }

  template <class vtype, class etype>
  inline std::pair<
    typename graph_traits< leda::GRAPH<vtype,etype> >::in_edge_iterator,
    typename graph_traits< leda::GRAPH<vtype,etype> >::in_edge_iterator >  
  in_edges(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u, 
    const leda::GRAPH<vtype,etype>& g)
  {
    typedef typename graph_traits< leda::GRAPH<vtype,etype> >
      ::in_edge_iterator Iter;
    return std::make_pair( Iter(First_Adj_Edge(u,1)), Iter(0) );
  }

  template <class vtype, class etype>
  inline std::pair<
    typename graph_traits< leda::GRAPH<vtype,etype> >::adjacency_iterator,
    typename graph_traits< leda::GRAPH<vtype,etype> >::adjacency_iterator >  
  adjacent_vertices(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u, 
    const leda::GRAPH<vtype,etype>& g)
  {
    typedef typename graph_traits< leda::GRAPH<vtype,etype> >
      ::adjacency_iterator Iter;
    return std::make_pair( Iter(First_Adj_Edge(u,0)), Iter(0) );
  }

  template <class vtype, class etype>
  typename graph_traits< leda::GRAPH<vtype,etype> >::vertices_size_type
  num_vertices(const leda::GRAPH<vtype,etype>& g)
  {
    return g.number_of_nodes();
  }  

  template <class vtype, class etype>
  typename graph_traits< leda::GRAPH<vtype,etype> >::edges_size_type
  num_edges(const leda::GRAPH<vtype,etype>& g)
  {
    return g.number_of_edges();
  }  

  template <class vtype, class etype>
  typename graph_traits< leda::GRAPH<vtype,etype> >::degree_size_type
  out_degree(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u, 
    const leda::GRAPH<vtype,etype>&)
  {
    return outdeg(u);
  }

  template <class vtype, class etype>
  typename graph_traits< leda::GRAPH<vtype,etype> >::degree_size_type
  in_degree(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u, 
    const leda::GRAPH<vtype,etype>&)
  {
    return indeg(u);
  }

  template <class vtype, class etype>
  typename graph_traits< leda::GRAPH<vtype,etype> >::degree_size_type
  degree(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u, 
    const leda::GRAPH<vtype,etype>&)
  {
    return outdeg(u) + indeg(u);
  }
  
  template <class vtype, class etype>
  typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor
  add_vertex(leda::GRAPH<vtype,etype>& g)
  {
    return g.new_node();
  }

  template <class vtype, class etype>
  typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor
  add_vertex(const vtype& vp, leda::GRAPH<vtype,etype>& g)
  {
    return g.new_node(vp);
  }

  // Hmm, LEDA doesn't have the equivalent of clear_vertex() -JGS
  // need to write an implementation
  template <class vtype, class etype>
  void clear_vertex(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u,
    leda::GRAPH<vtype,etype>& g)
  {
    g.del_node(u);
  }

  template <class vtype, class etype>
  void remove_vertex(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u,
    leda::GRAPH<vtype,etype>& g)
  {
    g.del_node(u);
  }

  template <class vtype, class etype>
  std::pair<
    typename graph_traits< leda::GRAPH<vtype,etype> >::edge_descriptor,
    bool>
  add_edge(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u,
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor v,
    leda::GRAPH<vtype,etype>& g)
  {
    return std::make_pair(g.new_edge(u, v), true);
  }

  template <class vtype, class etype>
  std::pair<
    typename graph_traits< leda::GRAPH<vtype,etype> >::edge_descriptor,
    bool>
  add_edge(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u,
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor v,
    const etype& et, 
    leda::GRAPH<vtype,etype>& g)
  {
    return std::make_pair(g.new_edge(u, v, et), true);
  }

  template <class vtype, class etype>
  void
  remove_edge(
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor u,
    typename graph_traits< leda::GRAPH<vtype,etype> >::vertex_descriptor v,
    leda::GRAPH<vtype,etype>& g)
  {
    typename graph_traits< leda::GRAPH<vtype,etype> >::out_edge_iterator 
      i,iend;
    for (boost::tie(i,iend) = out_edges(u,g); i != iend; ++i)
      if (target(*i,g) == v)
        g.del_edge(*i);
  }

  template <class vtype, class etype>
  void
  remove_edge(
    typename graph_traits< leda::GRAPH<vtype,etype> >::edge_descriptor e,
    leda::GRAPH<vtype,etype>& g)
  {
    g.del_edge(e);
  }

  //===========================================================================
  // property maps
  
  class leda_graph_id_map
    : public put_get_helper<int, leda_graph_id_map>
  {
  public:
    typedef readable_property_map_tag category;
    typedef int value_type;
    typedef int reference;
    typedef leda_node key_type;
    leda_graph_id_map() { }
    template <class T>
    long operator[](T x) const { return x->id(); }
  };
  template <class vtype, class etype>
  inline leda_graph_id_map
  get(vertex_index_t, const leda::GRAPH<vtype, etype>& g) {
    return leda_graph_id_map();
  }
  template <class vtype, class etype>
  inline leda_graph_id_map
  get(edge_index_t, const leda::GRAPH<vtype, etype>& g) {
    return leda_graph_id_map();
  }

  template <class Tag>
  struct leda_property_map { };

  template <>
  struct leda_property_map<vertex_index_t> {
    template <class vtype, class etype>
    struct bind_ {
      typedef leda_graph_id_map type;
      typedef leda_graph_id_map const_type;
    };
  };
  template <>
  struct leda_property_map<edge_index_t> {
    template <class vtype, class etype>
    struct bind_ {
      typedef leda_graph_id_map type;
      typedef leda_graph_id_map const_type;
    };
  };


  template <class Data, class DataRef, class GraphPtr>
  class leda_graph_data_map
    : public put_get_helper<DataRef, 
                            leda_graph_data_map<Data,DataRef,GraphPtr> >
  {
  public:
    typedef Data value_type;
    typedef DataRef reference;
    typedef void key_type;
    typedef lvalue_property_map_tag category;
    leda_graph_data_map(GraphPtr g) : m_g(g) { }
    template <class NodeOrEdge>
    DataRef operator[](NodeOrEdge x) const { return (*m_g)[x]; }
  protected:
    GraphPtr m_g;
  };

  template <>
  struct leda_property_map<vertex_all_t> {
    template <class vtype, class etype>
    struct bind_ {
      typedef leda_graph_data_map<vtype, vtype&, leda::GRAPH<vtype, etype>*> type;
      typedef leda_graph_data_map<vtype, const vtype&, 
        const leda::GRAPH<vtype, etype>*> const_type;
    };
  };  
  template <class vtype, class etype >
  inline typename property_map< leda::GRAPH<vtype, etype>, vertex_all_t>::type
  get(vertex_all_t, leda::GRAPH<vtype, etype>& g) {
    typedef typename property_map< leda::GRAPH<vtype, etype>, vertex_all_t>::type 
      pmap_type;
    return pmap_type(&g);
  }
  template <class vtype, class etype >
  inline typename property_map< leda::GRAPH<vtype, etype>, vertex_all_t>::const_type
  get(vertex_all_t, const leda::GRAPH<vtype, etype>& g) {
    typedef typename property_map< leda::GRAPH<vtype, etype>, 
      vertex_all_t>::const_type pmap_type;
    return pmap_type(&g);
  }

  template <>
  struct leda_property_map<edge_all_t> {
    template <class vtype, class etype>
    struct bind_ {
      typedef leda_graph_data_map<etype, etype&, leda::GRAPH<vtype, etype>*> type;
      typedef leda_graph_data_map<etype, const etype&, 
        const leda::GRAPH<vtype, etype>*> const_type;
    };
  };
  template <class vtype, class etype >
  inline typename property_map< leda::GRAPH<vtype, etype>, edge_all_t>::type
  get(edge_all_t, leda::GRAPH<vtype, etype>& g) {
    typedef typename property_map< leda::GRAPH<vtype, etype>, edge_all_t>::type 
      pmap_type;
    return pmap_type(&g);
  }
  template <class vtype, class etype >
  inline typename property_map< leda::GRAPH<vtype, etype>, edge_all_t>::const_type
  get(edge_all_t, const leda::GRAPH<vtype, etype>& g) {
    typedef typename property_map< leda::GRAPH<vtype, etype>, 
      edge_all_t>::const_type pmap_type;
    return pmap_type(&g);
  }

  // property map interface to the LEDA node_array class

  template <class E, class ERef, class NodeMapPtr>
  class leda_node_property_map
    : public put_get_helper<ERef, leda_node_property_map<E, ERef, NodeMapPtr> >
  {
  public:
    typedef E value_type;
    typedef ERef reference;
    typedef leda_node key_type;
    typedef lvalue_property_map_tag category;
    leda_node_property_map(NodeMapPtr a) : m_array(a) { }
    ERef operator[](leda_node n) const { return (*m_array)[n]; }
  protected:
    NodeMapPtr m_array;
  };
  template <class E>
  leda_node_property_map<E, const E&, const leda_node_array<E>*>
  make_leda_node_property_map(const leda_node_array<E>& a)
  {
    typedef leda_node_property_map<E, const E&, const leda_node_array<E>*>
      pmap_type;
    return pmap_type(&a);
  }
  template <class E>
  leda_node_property_map<E, E&, leda_node_array<E>*>
  make_leda_node_property_map(leda_node_array<E>& a)
  {
    typedef leda_node_property_map<E, E&, leda_node_array<E>*> pmap_type;
    return pmap_type(&a);
  }

  template <class E>
  leda_node_property_map<E, const E&, const leda_node_map<E>*>
  make_leda_node_property_map(const leda_node_map<E>& a)
  {
    typedef leda_node_property_map<E,const E&,const leda_node_map<E>*> 
      pmap_type;
    return pmap_type(&a);
  }
  template <class E>
  leda_node_property_map<E, E&, leda_node_map<E>*>
  make_leda_node_property_map(leda_node_map<E>& a)
  {
    typedef leda_node_property_map<E, E&, leda_node_map<E>*> pmap_type;
    return pmap_type(&a);
  }

  // g++ 'enumeral_type' in template unification not implemented workaround
  template <class vtype, class etype, class Tag>
  struct property_map<leda::GRAPH<vtype, etype>, Tag> {
    typedef typename 
      leda_property_map<Tag>::template bind_<vtype, etype> map_gen;
    typedef typename map_gen::type type;
    typedef typename map_gen::const_type const_type;
  };

  template <class vtype, class etype, class PropertyTag, class Key>
  inline
  typename boost::property_traits<
    typename boost::property_map<leda::GRAPH<vtype, etype>,PropertyTag>::const_type
  >::value_type
  get(PropertyTag p, const leda::GRAPH<vtype, etype>& g, const Key& key) {
    return get(get(p, g), key);
  }
  
  template <class vtype, class etype, class PropertyTag, class Key,class Value>
  inline void
  put(PropertyTag p, leda::GRAPH<vtype, etype>& g, 
      const Key& key, const Value& value)
  {
    typedef typename property_map<leda::GRAPH<vtype, etype>, PropertyTag>::type Map;
    Map pmap = get(p, g);
    put(pmap, key, value);
  }

} // namespace boost


#endif // BOOST_GRAPH_LEDA_HPP
