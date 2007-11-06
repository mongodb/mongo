//
//=======================================================================
// Copyright 1997-2001 University of Notre Dame.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================
//

#ifndef BOOST_INCREMENTAL_COMPONENTS_HPP
#define BOOST_INCREMENTAL_COMPONENTS_HPP

#include <boost/detail/iterator.hpp>
#include <boost/graph/detail/incremental_components.hpp>

namespace boost {

  // A connected component algorithm for the case when dynamically
  // adding (but not removing) edges is common.  The
  // incremental_components() function is a preparing operation. Call
  // same_component to check whether two vertices are in the same
  // component, or use disjoint_set::find_set to determine the
  // representative for a vertex.

  // This version of connected components does not require a full
  // Graph. Instead, it just needs an edge list, where the vertices of
  // each edge need to be of integer type. The edges are assumed to
  // be undirected. The other difference is that the result is stored in
  // a container, instead of just a decorator.  The container should be
  // empty before the algorithm is called. It will grow during the
  // course of the algorithm. The container must be a model of
  // BackInsertionSequence and RandomAccessContainer
  // (std::vector is a good choice). After running the algorithm the
  // index container will map each vertex to the representative
  // vertex of the component to which it belongs.
  //
  // Adapted from an implementation by Alex Stepanov. The disjoint
  // sets data structure is from Tarjan's "Data Structures and Network
  // Algorithms", and the application to connected components is
  // similar to the algorithm described in Ch. 22 of "Intro to
  // Algorithms" by Cormen, et. all.
  //  
  // RankContainer is a random accessable container (operator[] is
  // defined) with a value type that can represent an integer part of
  // a binary log of the value type of the corresponding
  // ParentContainer (char is always enough) its size_type is no less
  // than the size_type of the corresponding ParentContainer

  // An implementation of disjoint sets can be found in
  // boost/pending/disjoint_sets.hpp
  
  template <class EdgeListGraph, class DisjointSets>
  void incremental_components(EdgeListGraph& g, DisjointSets& ds)
  {
    typename graph_traits<EdgeListGraph>::edge_iterator e, end;
    for (tie(e,end) = edges(g); e != end; ++e)
      ds.union_set(source(*e,g),target(*e,g));
  }
  
  template <class ParentIterator>
  void compress_components(ParentIterator first, ParentIterator last)
  {
    for (ParentIterator current = first; current != last; ++current) 
      detail::find_representative_with_full_compression(first, current-first);
  }
  
  template <class ParentIterator>
  typename boost::detail::iterator_traits<ParentIterator>::difference_type
  component_count(ParentIterator first, ParentIterator last)
  {
    std::ptrdiff_t count = 0;
    for (ParentIterator current = first; current != last; ++current) 
      if (*current == current - first) ++count; 
    return count;
  }
  
  // This algorithm can be applied to the result container of the
  // connected_components algorithm to normalize
  // the components.
  template <class ParentIterator>
  void normalize_components(ParentIterator first, ParentIterator last)
  {
    for (ParentIterator current = first; current != last; ++current) 
      detail::normalize_node(first, current - first);
  }
  
  template <class VertexListGraph, class DisjointSets> 
  void initialize_incremental_components(VertexListGraph& G, DisjointSets& ds)
  {
    typename graph_traits<VertexListGraph>
      ::vertex_iterator v, vend;
    for (tie(v, vend) = vertices(G); v != vend; ++v)
      ds.make_set(*v);
  }

  template <class Vertex, class DisjointSet>
  inline bool same_component(Vertex u, Vertex v, DisjointSet& ds)
  {
    return ds.find_set(u) == ds.find_set(v);
  }

  // considering changing the so that it initializes with a pair of
  // vertex iterators and a parent PA.
  
  template <class IndexT>
  class component_index
  {
  public://protected: (avoid friends for now)
    typedef std::vector<IndexT> MyIndexContainer;
    MyIndexContainer header;
    MyIndexContainer index;
    typedef typename MyIndexContainer::size_type SizeT;
    typedef typename MyIndexContainer::const_iterator IndexIter;
  public:
    typedef detail::component_iterator<IndexIter, IndexT, SizeT> 
      component_iterator;
    class component {
      friend class component_index;
    protected:
      IndexT number;
      const component_index<IndexT>* comp_ind_ptr;
      component(IndexT i, const component_index<IndexT>* p) 
        : number(i), comp_ind_ptr(p) {}
    public:
      typedef component_iterator iterator;
      typedef component_iterator const_iterator;
      typedef IndexT value_type;
      iterator begin() const {
        return iterator( comp_ind_ptr->index.begin(),
                         (comp_ind_ptr->header)[number] );
      }
      iterator end() const {
        return iterator( comp_ind_ptr->index.begin(), 
                         comp_ind_ptr->index.size() );
      }
    };
    typedef SizeT size_type;
    typedef component value_type;
    
#if defined(BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS)
    template <class Iterator>
    component_index(Iterator first, Iterator last) 
    : index(std::distance(first, last))
    { 
      std::copy(first, last, index.begin());
      detail::construct_component_index(index, header);
    }
#else
    template <class Iterator>
    component_index(Iterator first, Iterator last) 
      : index(first, last)
    { 
      detail::construct_component_index(index, header);
    }
#endif

    component operator[](IndexT i) const {
      return component(i, this);
    }
    SizeT size() const {
      return header.size();
    }
    
  };

} // namespace boost

#endif // BOOST_INCREMENTAL_COMPONENTS_HPP
