// Copyright 2004 The Trustees of Indiana University.

// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor
//           Andrew Lumsdaine
#ifndef BOOST_GRAPH_CIRCLE_LAYOUT_HPP
#define BOOST_GRAPH_CIRCLE_LAYOUT_HPP
#include <cmath>
#include <utility>
#include <boost/graph/graph_traits.hpp>

namespace boost {
  /** 
   * \brief Layout the graph with the vertices at the points of a regular
   * n-polygon. 
   *
   * The distance from the center of the polygon to each point is
   * determined by the @p radius parameter. The @p position parameter
   * must be an Lvalue Property Map whose value type is a class type
   * containing @c x and @c y members that will be set to the @c x and
   * @c y coordinates.
   */
  template<typename VertexListGraph, typename PositionMap, typename Radius>
  void 
  circle_graph_layout(const VertexListGraph& g, PositionMap position,
                      Radius radius)
  {
    const double pi = 3.14159;

#ifndef BOOST_NO_STDC_NAMESPACE
    using std::sin;
    using std::cos;
#endif // BOOST_NO_STDC_NAMESPACE

    typedef typename graph_traits<VertexListGraph>::vertices_size_type 
      vertices_size_type;

    vertices_size_type n = num_vertices(g);
    
    typedef typename graph_traits<VertexListGraph>::vertex_iterator 
      vertex_iterator;

    vertices_size_type i = 0;
    for(std::pair<vertex_iterator, vertex_iterator> v = vertices(g); 
        v.first != v.second; ++v.first, ++i) {
      position[*v.first].x = radius * cos(i * 2 * pi / n);
      position[*v.first].y = radius * sin(i * 2 * pi / n);
    }
  }
} // end namespace boost

#endif // BOOST_GRAPH_CIRCLE_LAYOUT_HPP
