// Copyright 2004 The Trustees of Indiana University.

// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Douglas Gregor
//           Andrew Lumsdaine
#ifndef BOOST_GRAPH_KAMADA_KAWAI_SPRING_LAYOUT_HPP
#define BOOST_GRAPH_KAMADA_KAWAI_SPRING_LAYOUT_HPP

#include <boost/graph/graph_traits.hpp>
#include <boost/graph/johnson_all_pairs_shortest.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <utility>
#include <iterator>
#include <vector>
#include <boost/limits.hpp>
#include <cmath>

namespace boost {
  namespace detail { namespace graph {
    /**
     * Denotes an edge or display area side length used to scale a
     * Kamada-Kawai drawing.
     */
    template<bool Edge, typename T>
    struct edge_or_side
    {
      explicit edge_or_side(T value) : value(value) {}

      T value;
    };

    /**
     * Compute the edge length from an edge length. This is trivial.
     */
    template<typename Graph, typename DistanceMap, typename IndexMap, 
             typename T>
    T compute_edge_length(const Graph&, DistanceMap, IndexMap, 
                          edge_or_side<true, T> length)
    { return length.value; }

    /**
     * Compute the edge length based on the display area side
       length. We do this by dividing the side length by the largest
       shortest distance between any two vertices in the graph.
     */
    template<typename Graph, typename DistanceMap, typename IndexMap, 
             typename T>
    T
    compute_edge_length(const Graph& g, DistanceMap distance, IndexMap index,
                        edge_or_side<false, T> length)
    {
      T result(0);

      typedef typename graph_traits<Graph>::vertex_iterator vertex_iterator;

      for (vertex_iterator ui = vertices(g).first, end = vertices(g).second;
           ui != end; ++ui) {
        vertex_iterator vi = ui;
        for (++vi; vi != end; ++vi) {
          T dij = distance[get(index, *ui)][get(index, *vi)];
          if (dij > result) result = dij;
        }
      }
      return length.value / result;
    }

    /**
     * Implementation of the Kamada-Kawai spring layout algorithm.
     */
    template<typename Graph, typename PositionMap, typename WeightMap,
             typename EdgeOrSideLength, typename Done,
             typename VertexIndexMap, typename DistanceMatrix,
             typename SpringStrengthMatrix, typename PartialDerivativeMap>
    struct kamada_kawai_spring_layout_impl
    {
      typedef typename property_traits<WeightMap>::value_type weight_type;
      typedef std::pair<weight_type, weight_type> deriv_type;
      typedef typename graph_traits<Graph>::vertex_iterator vertex_iterator;
      typedef typename graph_traits<Graph>::vertex_descriptor
        vertex_descriptor;

      kamada_kawai_spring_layout_impl(
        const Graph& g, 
        PositionMap position,
        WeightMap weight, 
        EdgeOrSideLength edge_or_side_length,
        Done done,
        weight_type spring_constant,
        VertexIndexMap index,
        DistanceMatrix distance,
        SpringStrengthMatrix spring_strength,
        PartialDerivativeMap partial_derivatives)
        : g(g), position(position), weight(weight), 
          edge_or_side_length(edge_or_side_length), done(done),
          spring_constant(spring_constant), index(index), distance(distance),
          spring_strength(spring_strength), 
          partial_derivatives(partial_derivatives) {}

      // Compute contribution of vertex i to the first partial
      // derivatives (dE/dx_m, dE/dy_m) (for vertex m)
      deriv_type
      compute_partial_derivative(vertex_descriptor m, vertex_descriptor i)
      {
#ifndef BOOST_NO_STDC_NAMESPACE
        using std::sqrt;
#endif // BOOST_NO_STDC_NAMESPACE

        deriv_type result(0, 0);
        if (i != m) {
          weight_type x_diff = position[m].x - position[i].x;
          weight_type y_diff = position[m].y - position[i].y;
          weight_type dist = sqrt(x_diff * x_diff + y_diff * y_diff);
          result.first = spring_strength[get(index, m)][get(index, i)] 
            * (x_diff - distance[get(index, m)][get(index, i)]*x_diff/dist);
          result.second = spring_strength[get(index, m)][get(index, i)] 
            * (y_diff - distance[get(index, m)][get(index, i)]*y_diff/dist);
        }

        return result;
      }

      // Compute partial derivatives dE/dx_m and dE/dy_m
      deriv_type 
      compute_partial_derivatives(vertex_descriptor m)
      {
#ifndef BOOST_NO_STDC_NAMESPACE
        using std::sqrt;
#endif // BOOST_NO_STDC_NAMESPACE

        deriv_type result(0, 0);

        // TBD: looks like an accumulate to me
        std::pair<vertex_iterator, vertex_iterator> verts = vertices(g);
        for (/* no init */; verts.first != verts.second; ++verts.first) {
          vertex_descriptor i = *verts.first;
          deriv_type deriv = compute_partial_derivative(m, i);
          result.first += deriv.first;
          result.second += deriv.second;
        }

        return result;
      }

      // The actual Kamada-Kawai spring layout algorithm implementation
      bool run()
      {
#ifndef BOOST_NO_STDC_NAMESPACE
        using std::sqrt;
#endif // BOOST_NO_STDC_NAMESPACE

        // Compute d_{ij} and place it in the distance matrix
        if (!johnson_all_pairs_shortest_paths(g, distance, index, weight, 
                                              weight_type(0)))
          return false;

        // Compute L based on side length (if needed), or retrieve L
        weight_type edge_length = 
          detail::graph::compute_edge_length(g, distance, index,
                                             edge_or_side_length);
        
        // Compute l_{ij} and k_{ij}
        const weight_type K = spring_constant;
        vertex_iterator ui, end = vertices(g).second;
        for (ui = vertices(g).first; ui != end; ++ui) {
          vertex_iterator vi = ui;
          for (++vi; vi != end; ++vi) {
            weight_type dij = distance[get(index, *ui)][get(index, *vi)];
            if (dij == (std::numeric_limits<weight_type>::max)())
              return false;
            distance[get(index, *ui)][get(index, *vi)] = edge_length * dij;
            distance[get(index, *vi)][get(index, *ui)] = edge_length * dij;
            spring_strength[get(index, *ui)][get(index, *vi)] = K/(dij*dij);
            spring_strength[get(index, *vi)][get(index, *ui)] = K/(dij*dij);
          }
        }
        
        // Compute Delta_i and find max
        vertex_descriptor p = *vertices(g).first;
        weight_type delta_p(0);

        for (ui = vertices(g).first; ui != end; ++ui) {
          deriv_type deriv = compute_partial_derivatives(*ui);
          put(partial_derivatives, *ui, deriv);

          weight_type delta = 
            sqrt(deriv.first*deriv.first + deriv.second*deriv.second);

          if (delta > delta_p) {
            p = *ui;
            delta_p = delta;
          }
        }

        while (!done(delta_p, p, g, true)) {
          // The contribution p makes to the partial derivatives of
          // each vertex. Computing this (at O(n) cost) allows us to
          // update the delta_i values in O(n) time instead of O(n^2)
          // time.
          std::vector<deriv_type> p_partials(num_vertices(g));
          for (ui = vertices(g).first; ui != end; ++ui) {
            vertex_descriptor i = *ui;
            p_partials[get(index, i)] = compute_partial_derivative(i, p);
          }

          do {
            // Compute the 4 elements of the Jacobian
            weight_type dE_dx_dx = 0, dE_dx_dy = 0, dE_dy_dx = 0, dE_dy_dy = 0;
            for (ui = vertices(g).first; ui != end; ++ui) {
              vertex_descriptor i = *ui;
              if (i != p) {
                weight_type x_diff = position[p].x - position[i].x;
                weight_type y_diff = position[p].y - position[i].y;
                weight_type dist = sqrt(x_diff * x_diff + y_diff * y_diff);
                weight_type dist_cubed = dist * dist * dist;
                weight_type k_mi = spring_strength[get(index,p)][get(index,i)];
                weight_type l_mi = distance[get(index, p)][get(index, i)];
                dE_dx_dx += k_mi * (1 - (l_mi * y_diff * y_diff)/dist_cubed);
                dE_dx_dy += k_mi * l_mi * x_diff * y_diff / dist_cubed;
                dE_dy_dx += k_mi * l_mi * x_diff * y_diff / dist_cubed;
                dE_dy_dy += k_mi * (1 - (l_mi * x_diff * x_diff)/dist_cubed);
              }
            }

            // Solve for delta_x and delta_y
            weight_type dE_dx = get(partial_derivatives, p).first;
            weight_type dE_dy = get(partial_derivatives, p).second;

            weight_type delta_x = 
              (dE_dx_dy * dE_dy - dE_dy_dy * dE_dx)
              / (dE_dx_dx * dE_dy_dy - dE_dx_dy * dE_dy_dx);

            weight_type delta_y = 
              (dE_dx_dx * dE_dy - dE_dy_dx * dE_dx)
              / (dE_dy_dx * dE_dx_dy - dE_dx_dx * dE_dy_dy);


            // Move p by (delta_x, delta_y)
            position[p].x += delta_x;
            position[p].y += delta_y;

            // Recompute partial derivatives and delta_p
            deriv_type deriv = compute_partial_derivatives(p);
            put(partial_derivatives, p, deriv);

            delta_p = 
              sqrt(deriv.first*deriv.first + deriv.second*deriv.second);
          } while (!done(delta_p, p, g, false));

          // Select new p by updating each partial derivative and delta
          vertex_descriptor old_p = p;
          for (ui = vertices(g).first; ui != end; ++ui) {
            deriv_type old_deriv_p = p_partials[get(index, *ui)];
            deriv_type old_p_partial = 
              compute_partial_derivative(*ui, old_p);
            deriv_type deriv = get(partial_derivatives, *ui);

            deriv.first += old_p_partial.first - old_deriv_p.first;
            deriv.second += old_p_partial.second - old_deriv_p.second;

            put(partial_derivatives, *ui, deriv);
            weight_type delta = 
              sqrt(deriv.first*deriv.first + deriv.second*deriv.second);

            if (delta > delta_p) {
              p = *ui;
              delta_p = delta;
            }
          }
        }

        return true;
      }

      const Graph& g; 
      PositionMap position;
      WeightMap weight; 
      EdgeOrSideLength edge_or_side_length;
      Done done;
      weight_type spring_constant;
      VertexIndexMap index;
      DistanceMatrix distance;
      SpringStrengthMatrix spring_strength;
      PartialDerivativeMap partial_derivatives;
    };
  } } // end namespace detail::graph

  /// States that the given quantity is an edge length.
  template<typename T> 
  inline detail::graph::edge_or_side<true, T>
  edge_length(T x) 
  { return detail::graph::edge_or_side<true, T>(x); }

  /// States that the given quantity is a display area side length.
  template<typename T> 
  inline detail::graph::edge_or_side<false, T>
  side_length(T x) 
  { return detail::graph::edge_or_side<false, T>(x); }

  /** 
   * \brief Determines when to terminate layout of a particular graph based
   * on a given relative tolerance. 
   */
  template<typename T = double>
  struct layout_tolerance
  {
    layout_tolerance(const T& tolerance = T(0.001))
      : tolerance(tolerance), last_energy((std::numeric_limits<T>::max)()),
        last_local_energy((std::numeric_limits<T>::max)()) { }

    template<typename Graph>
    bool 
    operator()(T delta_p, 
               typename boost::graph_traits<Graph>::vertex_descriptor p,
               const Graph& g,
               bool global)
    {
      if (global) {
        if (last_energy == (std::numeric_limits<T>::max)()) {
          last_energy = delta_p;
          return false;
        }
          
        T diff = last_energy - delta_p;
        if (diff < T(0)) diff = -diff;
        bool done = (delta_p == T(0) || diff / last_energy < tolerance);
        last_energy = delta_p;
        return done;
      } else {
        if (last_local_energy == (std::numeric_limits<T>::max)()) {
          last_local_energy = delta_p;
          return delta_p == T(0);
        }
          
        T diff = last_local_energy - delta_p;
        bool done = (delta_p == T(0) || (diff / last_local_energy) < tolerance);
        last_local_energy = delta_p;
        return done;
      }
    }

  private:
    T tolerance;
    T last_energy;
    T last_local_energy;
  };

  /** \brief Kamada-Kawai spring layout for undirected graphs.
   *
   * This algorithm performs graph layout (in two dimensions) for
   * connected, undirected graphs. It operates by relating the layout
   * of graphs to a dynamic spring system and minimizing the energy
   * within that system. The strength of a spring between two vertices
   * is inversely proportional to the square of the shortest distance
   * (in graph terms) between those two vertices. Essentially,
   * vertices that are closer in the graph-theoretic sense (i.e., by
   * following edges) will have stronger springs and will therefore be
   * placed closer together.
   *
   * Prior to invoking this algorithm, it is recommended that the
   * vertices be placed along the vertices of a regular n-sided
   * polygon.
   *
   * \param g (IN) must be a model of Vertex List Graph, Edge List
   * Graph, and Incidence Graph and must be undirected.
   *
   * \param position (OUT) must be a model of Lvalue Property Map,
   * where the value type is a class containing fields @c x and @c y
   * that will be set to the @c x and @c y coordinates of each vertex.
   *
   * \param weight (IN) must be a model of Readable Property Map,
   * which provides the weight of each edge in the graph @p g.
   *
   * \param edge_or_side_length (IN) provides either the unit length
   * @c e of an edge in the layout or the length of a side @c s of the
   * display area, and must be either @c boost::edge_length(e) or @c
   * boost::side_length(s), respectively.
   *
   * \param done (IN) is a 4-argument function object that is passed
   * the current value of delta_p (i.e., the energy of vertex @p p),
   * the vertex @p p, the graph @p g, and a boolean flag indicating
   * whether @p delta_p is the maximum energy in the system (when @c
   * true) or the energy of the vertex being moved. Defaults to @c
   * layout_tolerance instantiated over the value type of the weight
   * map.
   *
   * \param spring_constant (IN) is the constant multiplied by each
   * spring's strength. Larger values create systems with more energy
   * that can take longer to stabilize; smaller values create systems
   * with less energy that stabilize quickly but do not necessarily
   * result in pleasing layouts. The default value is 1.
   *
   * \param index (IN) is a mapping from vertices to index values
   * between 0 and @c num_vertices(g). The default is @c
   * get(vertex_index,g).
   *
   * \param distance (UTIL/OUT) will be used to store the distance
   * from every vertex to every other vertex, which is computed in the
   * first stages of the algorithm. This value's type must be a model
   * of BasicMatrix with value type equal to the value type of the
   * weight map. The default is a a vector of vectors.
   *
   * \param spring_strength (UTIL/OUT) will be used to store the
   * strength of the spring between every pair of vertices. This
   * value's type must be a model of BasicMatrix with value type equal
   * to the value type of the weight map. The default is a a vector of
   * vectors.
   *
   * \param partial_derivatives (UTIL) will be used to store the
   * partial derivates of each vertex with respect to the @c x and @c
   * y coordinates. This must be a Read/Write Property Map whose value
   * type is a pair with both types equivalent to the value type of
   * the weight map. The default is an iterator property map.
   *
   * \returns @c true if layout was successful or @c false if a
   * negative weight cycle was detected.
   */
  template<typename Graph, typename PositionMap, typename WeightMap,
           typename T, bool EdgeOrSideLength, typename Done,
           typename VertexIndexMap, typename DistanceMatrix,
           typename SpringStrengthMatrix, typename PartialDerivativeMap>
  bool 
  kamada_kawai_spring_layout(
    const Graph& g, 
    PositionMap position,
    WeightMap weight, 
    detail::graph::edge_or_side<EdgeOrSideLength, T> edge_or_side_length,
    Done done,
    typename property_traits<WeightMap>::value_type spring_constant,
    VertexIndexMap index,
    DistanceMatrix distance,
    SpringStrengthMatrix spring_strength,
    PartialDerivativeMap partial_derivatives)
  {
    BOOST_STATIC_ASSERT((is_convertible<
                           typename graph_traits<Graph>::directed_category*,
                           undirected_tag*
                         >::value));

    detail::graph::kamada_kawai_spring_layout_impl<
      Graph, PositionMap, WeightMap, 
      detail::graph::edge_or_side<EdgeOrSideLength, T>, Done, VertexIndexMap, 
      DistanceMatrix, SpringStrengthMatrix, PartialDerivativeMap>
      alg(g, position, weight, edge_or_side_length, done, spring_constant,
          index, distance, spring_strength, partial_derivatives);
    return alg.run();
  }

  /**
   * \overload
   */
  template<typename Graph, typename PositionMap, typename WeightMap,
           typename T, bool EdgeOrSideLength, typename Done, 
           typename VertexIndexMap>
  bool 
  kamada_kawai_spring_layout(
    const Graph& g, 
    PositionMap position,
    WeightMap weight, 
    detail::graph::edge_or_side<EdgeOrSideLength, T> edge_or_side_length,
    Done done,
    typename property_traits<WeightMap>::value_type spring_constant,
    VertexIndexMap index)
  {
    typedef typename property_traits<WeightMap>::value_type weight_type;

    typename graph_traits<Graph>::vertices_size_type n = num_vertices(g);
    typedef std::vector<weight_type> weight_vec;

    std::vector<weight_vec> distance(n, weight_vec(n));
    std::vector<weight_vec> spring_strength(n, weight_vec(n));
    std::vector<std::pair<weight_type, weight_type> > partial_derivatives(n);

    return 
      kamada_kawai_spring_layout(
        g, position, weight, edge_or_side_length, done, spring_constant, index,
        distance.begin(),
        spring_strength.begin(),
        make_iterator_property_map(partial_derivatives.begin(), index,
                                   std::pair<weight_type, weight_type>()));
  }

  /**
   * \overload
   */
  template<typename Graph, typename PositionMap, typename WeightMap,
           typename T, bool EdgeOrSideLength, typename Done>
  bool 
  kamada_kawai_spring_layout(
    const Graph& g, 
    PositionMap position,
    WeightMap weight, 
    detail::graph::edge_or_side<EdgeOrSideLength, T> edge_or_side_length,
    Done done,
    typename property_traits<WeightMap>::value_type spring_constant)
  {
    return kamada_kawai_spring_layout(g, position, weight, edge_or_side_length,
                                      done, spring_constant, 
                                      get(vertex_index, g));
  }

  /**
   * \overload
   */
  template<typename Graph, typename PositionMap, typename WeightMap,
           typename T, bool EdgeOrSideLength, typename Done>
  bool 
  kamada_kawai_spring_layout(
    const Graph& g, 
    PositionMap position,
    WeightMap weight, 
    detail::graph::edge_or_side<EdgeOrSideLength, T> edge_or_side_length,
    Done done)
  {
    typedef typename property_traits<WeightMap>::value_type weight_type;
    return kamada_kawai_spring_layout(g, position, weight, edge_or_side_length,
                                      done, weight_type(1)); 
  }

  /**
   * \overload
   */
  template<typename Graph, typename PositionMap, typename WeightMap,
           typename T, bool EdgeOrSideLength>
  bool 
  kamada_kawai_spring_layout(
    const Graph& g, 
    PositionMap position,
    WeightMap weight, 
    detail::graph::edge_or_side<EdgeOrSideLength, T> edge_or_side_length)
  {
    typedef typename property_traits<WeightMap>::value_type weight_type;
    return kamada_kawai_spring_layout(g, position, weight, edge_or_side_length,
                                      layout_tolerance<weight_type>(),
                                      weight_type(1.0), 
                                      get(vertex_index, g));
  }
} // end namespace boost

#endif // BOOST_GRAPH_KAMADA_KAWAI_SPRING_LAYOUT_HPP
