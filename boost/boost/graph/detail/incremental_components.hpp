//=======================================================================
// Copyright 2002 Indiana University.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#ifndef BOOST_GRAPH_DETAIL_INCREMENTAL_COMPONENTS_HPP
#define BOOST_GRAPH_DETAIL_INCREMENTAL_COMPONENTS_HPP

#include <boost/operators.hpp>
#include <boost/pending/disjoint_sets.hpp>

namespace boost {

  namespace detail {

    //=========================================================================
    // Implementation detail of incremental_components


    //-------------------------------------------------------------------------
    // Helper functions for the component_index class
    
    // Record the representative vertices in the header array.
    // Representative vertices now point to the component number.
    
    template <class Parent, class OutputIterator, class Integer>
    inline void
    build_components_header(Parent p, 
                            OutputIterator header,
                            Integer num_nodes)
    {
      Parent component = p;
      Integer component_num = 0;
      for (Integer v = 0; v != num_nodes; ++v) 
        if (p[v] == v) {
          *header++ = v;
          component[v] = component_num++;
        }
    }
    
    
    // Pushes x onto the front of the list. The list is represented in
    // an array.
    template <class Next, class T, class V>
    inline void array_push_front(Next next, T& head, V x)
    {
      T tmp = head;
      head = x;
      next[x] = tmp;
    }
    
    
    // Create a linked list of the vertices in each component
    // by reusing the representative array.
    template <class Parent1, class Parent2, 
              class Integer>
    void
    link_components(Parent1 component, Parent2 header, 
                    Integer num_nodes, Integer num_components)
    {
      // Make the non-representative vertices point to their component
      Parent1 representative = component;
      for (Integer v = 0; v != num_nodes; ++v)
        if (component[v] >= num_components
            || header[component[v]] != v)
          component[v] = component[representative[v]];
      
      // initialize the "head" of the lists to "NULL"
      std::fill_n(header, num_components, num_nodes);
      
      // Add each vertex to the linked list for its component
      Parent1 next = component;
      for (Integer k = 0; k != num_nodes; ++k)
        array_push_front(next, header[component[k]], k);
    }
    

    
    template <class IndexContainer, class HeaderContainer>
    void
    construct_component_index(IndexContainer& index, HeaderContainer& header)
    {
      typedef typename IndexContainer::value_type Integer;
      build_components_header(index.begin(), 
                              std::back_inserter(header),
                              Integer(index.end() - index.begin()));
      
      link_components(index.begin(), header.begin(),
                      Integer(index.end() - index.begin()), 
                      Integer(header.end() - header.begin()));
    }
    
    
    
    template <class IndexIterator, class Integer, class Distance>
    class component_iterator 
      : boost::forward_iterator_helper< 
    component_iterator<IndexIterator,Integer,Distance>,
              Integer, Distance,Integer*, Integer&>
    {
    public:
      typedef component_iterator self;
      
      IndexIterator next;
      Integer node;
      
      typedef std::forward_iterator_tag iterator_category;
      typedef Integer value_type;
      typedef Integer& reference;
      typedef Integer* pointer;
      typedef Distance difference_type;
      
      component_iterator() {}
      component_iterator(IndexIterator x, Integer i) 
        : next(x), node(i) {}
      Integer operator*() const {
        return node;
      }
      self& operator++() {
        node = next[node];
        return *this;
      }
    };
    
    template <class IndexIterator, class Integer, class Distance>
    inline bool 
    operator==(const component_iterator<IndexIterator, Integer, Distance>& x,
               const component_iterator<IndexIterator, Integer, Distance>& y)
    {
      return x.node == y.node;
    }
  
  } // namespace detail
  
} // namespace detail

#endif // BOOST_GRAPH_DETAIL_INCREMENTAL_COMPONENTS_HPP
