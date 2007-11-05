// Copyright 2004 The Trustees of Indiana University.

// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Jeremiah Willcock
//           Douglas Gregor
//           Andrew Lumsdaine
#ifndef BOOST_GRAPH_GURSOY_ATUN_LAYOUT_HPP
#define BOOST_GRAPH_GURSOY_ATUN_LAYOUT_HPP

// Gursoy-Atun graph layout, based on:
// "Neighbourhood Preserving Load Balancing: A Self-Organizing Approach"
// in EuroPar 2000, p. 234 of LNCS 1900
// http://springerlink.metapress.com/link.asp?id=pcu07ew5rhexp9yt

#include <cmath>
#include <vector>
#include <exception>
#include <algorithm>

#include <boost/graph/visitors.hpp>
#include <boost/graph/properties.hpp>
#include <boost/random/uniform_01.hpp>
#include <boost/random/linear_congruential.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include <boost/graph/named_function_params.hpp>

namespace boost { 

namespace detail {

struct over_distance_limit : public std::exception {};

template <typename PositionMap, typename NodeDistanceMap,  typename Topology,
          typename Graph>
struct update_position_visitor {
  typedef typename Topology::point_type Point;
  PositionMap position_map;
  NodeDistanceMap node_distance;
  const Topology& space;
  Point input_vector;
  double distance_limit;
  double learning_constant;
  double falloff_ratio;

  typedef boost::on_examine_vertex event_filter;

  typedef typename graph_traits<Graph>::vertex_descriptor
    vertex_descriptor;

  update_position_visitor(PositionMap position_map,
                          NodeDistanceMap node_distance,
                          const Topology& space,
                          const Point& input_vector,
                          double distance_limit,
                          double learning_constant,
                          double falloff_ratio):
    position_map(position_map), node_distance(node_distance), 
    space(space),
    input_vector(input_vector), distance_limit(distance_limit),
    learning_constant(learning_constant), falloff_ratio(falloff_ratio) {}

  void operator()(vertex_descriptor v, const Graph&) const 
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::pow;
#endif

    if (get(node_distance, v) > distance_limit)
      throw over_distance_limit();
    Point old_position = get(position_map, v);
    double distance = get(node_distance, v);
    double fraction = 
      learning_constant * pow(falloff_ratio, distance * distance);
    put(position_map, v,
        space.move_position_toward(old_position, fraction, input_vector));
  }
};

template<typename EdgeWeightMap>
struct gursoy_shortest
{
  template<typename Graph, typename NodeDistanceMap, typename UpdatePosition>
  static inline void 
  run(const Graph& g, typename graph_traits<Graph>::vertex_descriptor s,
      NodeDistanceMap node_distance,  UpdatePosition& update_position,
      EdgeWeightMap weight)
  {
    boost::dijkstra_shortest_paths(g, s, weight_map(weight).
      visitor(boost::make_dijkstra_visitor(std::make_pair(
       boost::record_distances(node_distance, boost::on_edge_relaxed()),
        update_position))));
  }
};

template<>
struct gursoy_shortest<dummy_property_map>
{
  template<typename Graph, typename NodeDistanceMap, typename UpdatePosition>
  static inline void 
  run(const Graph& g, typename graph_traits<Graph>::vertex_descriptor s,
      NodeDistanceMap node_distance,  UpdatePosition& update_position,
      dummy_property_map)
  {
    boost::breadth_first_search(g, s,
      visitor(boost::make_bfs_visitor(std::make_pair(
        boost::record_distances(node_distance, boost::on_tree_edge()),
        update_position))));
  }
};

} // namespace detail

template <typename VertexListAndIncidenceGraph,  typename Topology,
          typename PositionMap, typename Diameter, typename VertexIndexMap, 
          typename EdgeWeightMap>
void 
gursoy_atun_step
  (const VertexListAndIncidenceGraph& graph,  
   const Topology& space,
   PositionMap position,
   Diameter diameter,
   double learning_constant,
   VertexIndexMap vertex_index_map,
   EdgeWeightMap weight)
{
#ifndef BOOST_NO_STDC_NAMESPACE
  using std::pow;
  using std::exp;
#endif

  typedef typename graph_traits<VertexListAndIncidenceGraph>::vertex_iterator
    vertex_iterator;
  typedef typename graph_traits<VertexListAndIncidenceGraph>::vertex_descriptor
    vertex_descriptor;
  typedef typename Topology::point_type point_type;
  vertex_iterator i, iend;
  std::vector<double> distance_from_input_vector(num_vertices(graph));
  typedef boost::iterator_property_map<std::vector<double>::iterator, 
                                       VertexIndexMap,
                                       double, double&>
    DistanceFromInputMap;
  DistanceFromInputMap distance_from_input(distance_from_input_vector.begin(),
                                           vertex_index_map);
  std::vector<double> node_distance_map_vector(num_vertices(graph));
  typedef boost::iterator_property_map<std::vector<double>::iterator, 
                                       VertexIndexMap,
                                       double, double&>
    NodeDistanceMap;
  NodeDistanceMap node_distance(node_distance_map_vector.begin(),
                                vertex_index_map);
  point_type input_vector = space.random_point();
  vertex_descriptor min_distance_loc 
    = graph_traits<VertexListAndIncidenceGraph>::null_vertex();
  double min_distance = 0.0;
  bool min_distance_unset = true;
  for (boost::tie(i, iend) = vertices(graph); i != iend; ++i) {
    double this_distance = space.distance(get(position, *i), input_vector);
    put(distance_from_input, *i, this_distance);
    if (min_distance_unset || this_distance < min_distance) {
      min_distance = this_distance;
      min_distance_loc = *i;
    }
    min_distance_unset = false;
  }
  assert (!min_distance_unset); // Graph must have at least one vertex
  boost::detail::update_position_visitor<
      PositionMap, NodeDistanceMap, Topology,
      VertexListAndIncidenceGraph> 
    update_position(position, node_distance, space,
                    input_vector, diameter, learning_constant, 
                    exp(-1. / (2 * diameter * diameter)));
  std::fill(node_distance_map_vector.begin(), node_distance_map_vector.end(), 0);
  try {
    typedef detail::gursoy_shortest<EdgeWeightMap> shortest;
    shortest::run(graph, min_distance_loc, node_distance, update_position,
                  weight);    
  } catch (detail::over_distance_limit) { 
    /* Thrown to break out of BFS or Dijkstra early */ 
  }
}

template <typename VertexListAndIncidenceGraph,  typename Topology,
          typename PositionMap, typename VertexIndexMap, 
          typename EdgeWeightMap>
void gursoy_atun_refine(const VertexListAndIncidenceGraph& graph,  
                        const Topology& space,
                        PositionMap position,
                        int nsteps,
                        double diameter_initial,
                        double diameter_final,
                        double learning_constant_initial,
                        double learning_constant_final,
                        VertexIndexMap vertex_index_map,
                        EdgeWeightMap weight) 
{
#ifndef BOOST_NO_STDC_NAMESPACE
  using std::pow;
  using std::exp;
#endif

  typedef typename graph_traits<VertexListAndIncidenceGraph>::vertex_iterator
    vertex_iterator;
  typedef typename graph_traits<VertexListAndIncidenceGraph>::vertex_descriptor
    vertex_descriptor;
  typedef typename Topology::point_type point_type;
  vertex_iterator i, iend;
  double diameter_ratio = (double)diameter_final / diameter_initial;
  double learning_constant_ratio = 
    learning_constant_final / learning_constant_initial;
  std::vector<double> distance_from_input_vector(num_vertices(graph));
  typedef boost::iterator_property_map<std::vector<double>::iterator, 
                                       VertexIndexMap,
                                       double, double&>
    DistanceFromInputMap;
  DistanceFromInputMap distance_from_input(distance_from_input_vector.begin(),
                                           vertex_index_map);
  std::vector<int> node_distance_map_vector(num_vertices(graph));
  typedef boost::iterator_property_map<std::vector<int>::iterator, 
                                       VertexIndexMap, double, double&>
    NodeDistanceMap;
  NodeDistanceMap node_distance(node_distance_map_vector.begin(),
                                vertex_index_map);
  for (int round = 0; round < nsteps; ++round) {
    double part_done = (double)round / (nsteps - 1);
    int diameter = (int)(diameter_initial * pow(diameter_ratio, part_done));
    double learning_constant = 
      learning_constant_initial * pow(learning_constant_ratio, part_done);
    gursoy_atun_step(graph, space, position, diameter, learning_constant, 
                     vertex_index_map, weight);
  }
}

template <typename VertexListAndIncidenceGraph,  typename Topology,
          typename PositionMap, typename VertexIndexMap, 
          typename EdgeWeightMap>
void gursoy_atun_layout(const VertexListAndIncidenceGraph& graph,  
                        const Topology& space,
                        PositionMap position,
                        int nsteps,
                        double diameter_initial,
                        double diameter_final,
                        double learning_constant_initial,
                        double learning_constant_final,
                        VertexIndexMap vertex_index_map,
                        EdgeWeightMap weight)
{
  typedef typename graph_traits<VertexListAndIncidenceGraph>::vertex_iterator
    vertex_iterator;
  vertex_iterator i, iend;
  for (boost::tie(i, iend) = vertices(graph); i != iend; ++i) {
    put(position, *i, space.random_point());
  }
  gursoy_atun_refine(graph, space,
                     position, nsteps,
                     diameter_initial, diameter_final, 
                     learning_constant_initial, learning_constant_final,
                     vertex_index_map, weight);
}

template <typename VertexListAndIncidenceGraph,  typename Topology,
          typename PositionMap, typename VertexIndexMap>
void gursoy_atun_layout(const VertexListAndIncidenceGraph& graph,  
                        const Topology& space,
                        PositionMap position,
                        int nsteps,
                        double diameter_initial,
                        double diameter_final,
                        double learning_constant_initial,
                        double learning_constant_final,
                        VertexIndexMap vertex_index_map)
{
  gursoy_atun_layout(graph, space, position, nsteps, 
                     diameter_initial, diameter_final, 
                     learning_constant_initial, learning_constant_final, 
                     vertex_index_map, dummy_property_map());
}

template <typename VertexListAndIncidenceGraph, typename Topology,
          typename PositionMap>
void gursoy_atun_layout(const VertexListAndIncidenceGraph& graph,  
                        const Topology& space,
                        PositionMap position,
                        int nsteps,
                        double diameter_initial,
                        double diameter_final = 1.0,
                        double learning_constant_initial = 0.8,
                        double learning_constant_final = 0.2)
{ 
  gursoy_atun_layout(graph, space, position, nsteps, diameter_initial,
                     diameter_final, learning_constant_initial,
                     learning_constant_final, get(vertex_index, graph)); 
}

template <typename VertexListAndIncidenceGraph, typename Topology,
          typename PositionMap>
void gursoy_atun_layout(const VertexListAndIncidenceGraph& graph,  
                        const Topology& space,
                        PositionMap position,
                        int nsteps)
{
#ifndef BOOST_NO_STDC_NAMESPACE
  using std::sqrt;
#endif

  gursoy_atun_layout(graph, space, position, nsteps, 
                     sqrt((double)num_vertices(graph)));
}

template <typename VertexListAndIncidenceGraph, typename Topology,
          typename PositionMap>
void gursoy_atun_layout(const VertexListAndIncidenceGraph& graph,  
                        const Topology& space,
                        PositionMap position)
{
  gursoy_atun_layout(graph, space, position, num_vertices(graph));
}

template<typename VertexListAndIncidenceGraph, typename Topology,
         typename PositionMap, typename P, typename T, typename R>
void 
gursoy_atun_layout(const VertexListAndIncidenceGraph& graph,  
                   const Topology& space,
                   PositionMap position,
                   const bgl_named_params<P,T,R>& params)
{
#ifndef BOOST_NO_STDC_NAMESPACE
  using std::sqrt;
#endif

  std::pair<double, double> diam(sqrt(double(num_vertices(graph))), 1.0);
  std::pair<double, double> learn(0.8, 0.2);
  gursoy_atun_layout(graph, space, position,
                     choose_param(get_param(params, iterations_t()),
                                  num_vertices(graph)),
                     choose_param(get_param(params, diameter_range_t()), 
                                  diam).first,
                     choose_param(get_param(params, diameter_range_t()), 
                                  diam).second,
                     choose_param(get_param(params, learning_constant_range_t()), 
                                  learn).first,
                     choose_param(get_param(params, learning_constant_range_t()), 
                                  learn).second,
                     choose_const_pmap(get_param(params, vertex_index), graph,
                                       vertex_index),
                     choose_param(get_param(params, edge_weight), 
                                  dummy_property_map()));
}

/***********************************************************
 * Topologies                                              *
 ***********************************************************/
template<std::size_t Dims>
class convex_topology 
{
  struct point 
  {
    point() { }
    double& operator[](std::size_t i) {return values[i];}
    const double& operator[](std::size_t i) const {return values[i];}

  private:
    double values[Dims];
  };

 public:
  typedef point point_type;

  double distance(point a, point b) const 
  {
    double dist = 0;
    for (std::size_t i = 0; i < Dims; ++i) {
      double diff = b[i] - a[i];
      dist += diff * diff;
    }
    // Exact properties of the distance are not important, as long as
    // < on what this returns matches real distances
    return dist;
  }

  point move_position_toward(point a, double fraction, point b) const 
  {
    point result;
    for (std::size_t i = 0; i < Dims; ++i)
      result[i] = a[i] + (b[i] - a[i]) * fraction;
    return result;
  }
};

template<std::size_t Dims,
         typename RandomNumberGenerator = minstd_rand>
class hypercube_topology : public convex_topology<Dims>
{
  typedef uniform_01<RandomNumberGenerator, double> rand_t;

 public:
  typedef typename convex_topology<Dims>::point_type point_type;

  explicit hypercube_topology(double scaling = 1.0) 
    : gen_ptr(new RandomNumberGenerator), rand(new rand_t(*gen_ptr)), 
      scaling(scaling) 
  { }

  hypercube_topology(RandomNumberGenerator& gen, double scaling = 1.0) 
    : gen_ptr(), rand(new rand_t(gen)), scaling(scaling) { }
                     
  point_type random_point() const 
  {
    point_type p;
    for (std::size_t i = 0; i < Dims; ++i)
      p[i] = (*rand)() * scaling;
    return p;
  }

 private:
  shared_ptr<RandomNumberGenerator> gen_ptr;
  shared_ptr<rand_t> rand;
  double scaling;
};

template<typename RandomNumberGenerator = minstd_rand>
class square_topology : public hypercube_topology<2, RandomNumberGenerator>
{
  typedef hypercube_topology<2, RandomNumberGenerator> inherited;

 public:
  explicit square_topology(double scaling = 1.0) : inherited(scaling) { }
  
  square_topology(RandomNumberGenerator& gen, double scaling = 1.0) 
    : inherited(gen, scaling) { }
};

template<typename RandomNumberGenerator = minstd_rand>
class cube_topology : public hypercube_topology<3, RandomNumberGenerator>
{
  typedef hypercube_topology<3, RandomNumberGenerator> inherited;

 public:
  explicit cube_topology(double scaling = 1.0) : inherited(scaling) { }
  
  cube_topology(RandomNumberGenerator& gen, double scaling = 1.0) 
    : inherited(gen, scaling) { }
};

template<std::size_t Dims,
         typename RandomNumberGenerator = minstd_rand>
class ball_topology : public convex_topology<Dims>
{
  typedef uniform_01<RandomNumberGenerator, double> rand_t;

 public:
  typedef typename convex_topology<Dims>::point_type point_type;

  explicit ball_topology(double radius = 1.0) 
    : gen_ptr(new RandomNumberGenerator), rand(new rand_t(*gen_ptr)), 
      radius(radius) 
  { }

  ball_topology(RandomNumberGenerator& gen, double radius = 1.0) 
    : gen_ptr(), rand(new rand_t(gen)), radius(radius) { }
                     
  point_type random_point() const 
  {
    point_type p;
    double dist_sum;
    do {
      dist_sum = 0.0;
      for (std::size_t i = 0; i < Dims; ++i) {
        double x = (*rand)() * 2*radius - radius;
        p[i] = x;
        dist_sum += x * x;
      }
    } while (dist_sum > radius*radius);
    return p;
  }

 private:
  shared_ptr<RandomNumberGenerator> gen_ptr;
  shared_ptr<rand_t> rand;
  double radius;
};

template<typename RandomNumberGenerator = minstd_rand>
class circle_topology : public ball_topology<2, RandomNumberGenerator>
{
  typedef ball_topology<2, RandomNumberGenerator> inherited;

 public:
  explicit circle_topology(double radius = 1.0) : inherited(radius) { }
  
  circle_topology(RandomNumberGenerator& gen, double radius = 1.0) 
    : inherited(gen, radius) { }
};

template<typename RandomNumberGenerator = minstd_rand>
class sphere_topology : public ball_topology<3, RandomNumberGenerator>
{
  typedef ball_topology<3, RandomNumberGenerator> inherited;

 public:
  explicit sphere_topology(double radius = 1.0) : inherited(radius) { }
  
  sphere_topology(RandomNumberGenerator& gen, double radius = 1.0) 
    : inherited(gen, radius) { }
};

template<typename RandomNumberGenerator = minstd_rand>
class heart_topology 
{
  // Heart is defined as the union of three shapes:
  // Square w/ corners (+-1000, -1000), (0, 0), (0, -2000)
  // Circle centered at (-500, -500) radius 500*sqrt(2)
  // Circle centered at (500, -500) radius 500*sqrt(2)
  // Bounding box (-1000, -2000) - (1000, 500*(sqrt(2) - 1))

  struct point 
  {
    point() { values[0] = 0.0; values[1] = 0.0; }
    point(double x, double y) { values[0] = x; values[1] = y; }

    double& operator[](std::size_t i)       { return values[i]; }
    double  operator[](std::size_t i) const { return values[i]; }

  private:
    double values[2];
  };

  bool in_heart(point p) const 
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::abs;
    using std::pow;
#endif

    if (p[1] < abs(p[0]) - 2000) return false; // Bottom
    if (p[1] <= -1000) return true; // Diagonal of square
    if (pow(p[0] - -500, 2) + pow(p[1] - -500, 2) <= 500000)
      return true; // Left circle
    if (pow(p[0] - 500, 2) + pow(p[1] - -500, 2) <= 500000)
      return true; // Right circle
    return false;
  }

  bool segment_within_heart(point p1, point p2) const 
  {
    // Assumes that p1 and p2 are within the heart
    if ((p1[0] < 0) == (p2[0] < 0)) return true; // Same side of symmetry line
    if (p1[0] == p2[0]) return true; // Vertical
    double slope = (p2[1] - p1[1]) / (p2[0] - p1[0]);
    double intercept = p1[1] - p1[0] * slope;
    if (intercept > 0) return false; // Crosses between circles
    return true;
  }

  typedef uniform_01<RandomNumberGenerator, double> rand_t;

 public:
  typedef point point_type;

  heart_topology() 
    : gen_ptr(new RandomNumberGenerator), rand(new rand_t(*gen_ptr)) { }

  heart_topology(RandomNumberGenerator& gen) 
    : gen_ptr(), rand(new rand_t(gen)) { }

  point random_point() const 
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::sqrt;
#endif

    point result;
    double sqrt2 = sqrt(2.);
    do {
      result[0] = (*rand)() * (1000 + 1000 * sqrt2) - (500 + 500 * sqrt2);
      result[1] = (*rand)() * (2000 + 500 * (sqrt2 - 1)) - 2000;
    } while (!in_heart(result));
    return result;
  }

  double distance(point a, point b) const 
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::sqrt;
#endif
    if (segment_within_heart(a, b)) {
      // Straight line
      return sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]));
    } else {
      // Straight line bending around (0, 0)
      return sqrt(a[0] * a[0] + a[1] * a[1]) + sqrt(b[0] * b[0] + b[1] * b[1]);
    }
  }

  point move_position_toward(point a, double fraction, point b) const 
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::sqrt;
#endif

    if (segment_within_heart(a, b)) {
      // Straight line
      return point(a[0] + (b[0] - a[0]) * fraction,
                   a[1] + (b[1] - a[1]) * fraction);
    } else {
      double distance_to_point_a = sqrt(a[0] * a[0] + a[1] * a[1]);
      double distance_to_point_b = sqrt(b[0] * b[0] + b[1] * b[1]);
      double location_of_point = distance_to_point_a / 
                                   (distance_to_point_a + distance_to_point_b);
      if (fraction < location_of_point)
        return point(a[0] * (1 - fraction / location_of_point), 
                     a[1] * (1 - fraction / location_of_point));
      else
        return point(
          b[0] * ((fraction - location_of_point) / (1 - location_of_point)),
          b[1] * ((fraction - location_of_point) / (1 - location_of_point)));
    }
  }

 private:
  shared_ptr<RandomNumberGenerator> gen_ptr;
  shared_ptr<rand_t> rand;
};

} // namespace boost

#endif // BOOST_GRAPH_GURSOY_ATUN_LAYOUT_HPP
