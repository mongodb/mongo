//=======================================================================
// Copyright 1997, 1998, 1999, 2000 University of Notre Dame.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek,
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================
#ifndef BOOST_GRAPH_RELAX_HPP
#define BOOST_GRAPH_RELAX_HPP

#include <functional>
#include <boost/limits.hpp> // for numeric limits
#include <boost/graph/graph_traits.hpp>
#include <boost/property_map.hpp>

namespace boost {

    // The following version of the plus functor prevents
    // problems due to overflow at positive infinity.

    template <class T>
    struct closed_plus
    {
      T operator()(const T& a, const T& b) const {
        using namespace std;
	T zero(0);
	T result = a + b;
	if (result < zero && a >= zero && b >= zero)
	  return (numeric_limits<T>::max)();
	return result;
      }
    };
    
    template <class Graph, class WeightMap, 
            class PredecessorMap, class DistanceMap, 
            class BinaryFunction, class BinaryPredicate>
    bool relax(typename graph_traits<Graph>::edge_descriptor e, 
               const Graph& g, const WeightMap& w, 
               PredecessorMap& p, DistanceMap& d, 
               const BinaryFunction& combine, const BinaryPredicate& compare)
    {
      typedef typename graph_traits<Graph>::directed_category DirCat;
      bool is_undirected = is_same<DirCat, undirected_tag>::value;
      typedef typename graph_traits<Graph>::vertex_descriptor Vertex;
      Vertex u = source(e, g), v = target(e, g);
      typedef typename property_traits<DistanceMap>::value_type D;
      typedef typename property_traits<WeightMap>::value_type W;
      D d_u = get(d, u), d_v = get(d, v);
      W w_e = get(w, e);
      
      if ( compare(combine(d_u, w_e), d_v) ) {
        put(d, v, combine(d_u, w_e));
        put(p, v, u);
        return true;
      } else if (is_undirected && compare(combine(d_v, w_e), d_u)) {
        put(d, u, combine(d_v, w_e));
        put(p, u, v);
        return true;
      } else
        return false;
    }
    
    template <class Graph, class WeightMap, 
      class PredecessorMap, class DistanceMap>
    bool relax(typename graph_traits<Graph>::edge_descriptor e,
               const Graph& g, WeightMap w, PredecessorMap p, DistanceMap d)
    {
      typedef typename property_traits<DistanceMap>::value_type D;
      typedef closed_plus<D> Combine;
      typedef std::less<D> Compare;
      return relax(e, g, w, p, d, Combine(), Compare());
    }

} // namespace boost

#endif /* BOOST_GRAPH_RELAX_HPP */
