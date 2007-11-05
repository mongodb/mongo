//=======================================================================
// Copyright 1997, 1998, 1999, 2000 University of Notre Dame.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#ifndef BOOST_GRAPH_NAMED_FUNCTION_PARAMS_HPP
#define BOOST_GRAPH_NAMED_FUNCTION_PARAMS_HPP

#include <boost/graph/properties.hpp>

namespace boost {

  struct distance_compare_t { };
  struct distance_combine_t { };
  struct distance_inf_t { };
  struct distance_zero_t { };
  struct buffer_param_t { };
  struct edge_copy_t { };
  struct vertex_copy_t { };
  struct vertex_isomorphism_t { };
  struct vertex_invariant_t { };
  struct vertex_invariant1_t { };
  struct vertex_invariant2_t { };
  struct edge_compare_t { };
  struct vertex_max_invariant_t { };
  struct orig_to_copy_t { };
  struct root_vertex_t { };
  struct attractive_force_t { };
  struct repulsive_force_t { };
  struct force_pairs_t { };
  struct cooling_t { };
  struct vertex_displacement_t { };
  struct iterations_t { };
  struct diameter_range_t { };
  struct learning_constant_range_t { };

  namespace detail {
    template <class T>
    struct wrap_ref {
      wrap_ref(T& r) : ref(r) {}
      T& ref;
    };
  }

  template <typename T, typename Tag, typename Base = no_property>
  struct bgl_named_params : public Base
  {
    typedef bgl_named_params self;
    typedef Base next_type;
    typedef Tag tag_type;
    typedef T value_type;
    bgl_named_params(T v) : m_value(v) { }
    bgl_named_params(T v, const Base& b) : Base(b), m_value(v) { }
    T m_value;

    template <typename WeightMap>
    bgl_named_params<WeightMap, edge_weight_t, self>
    weight_map(const WeightMap& pmap) const {
      typedef bgl_named_params<WeightMap, edge_weight_t, self> Params;
      return Params(pmap, *this);
    }

    template <typename WeightMap>
    bgl_named_params<WeightMap, edge_weight2_t, self>
    weight_map2(const WeightMap& pmap) const {
      typedef bgl_named_params<WeightMap, edge_weight2_t, self> Params;
      return Params(pmap, *this);
    }

    template <typename DistanceMap>
    bgl_named_params<DistanceMap, vertex_distance_t, self>
    distance_map(const DistanceMap& pmap) const {
      typedef bgl_named_params<DistanceMap, vertex_distance_t, self> Params;
      return Params(pmap, *this);
    }

    template <typename PredecessorMap>
    bgl_named_params<PredecessorMap, vertex_predecessor_t, self>
    predecessor_map(const PredecessorMap& pmap) const {
      typedef bgl_named_params<PredecessorMap, vertex_predecessor_t, self> 
        Params;
      return Params(pmap, *this);
    }

    template <typename RankMap>
    bgl_named_params<RankMap, vertex_rank_t, self>
    rank_map(const RankMap& pmap) const {
      typedef bgl_named_params<RankMap, vertex_rank_t, self> 
        Params;
      return Params(pmap, *this);
    }

    template <typename RootMap>
    bgl_named_params<RootMap, vertex_root_t, self>
    root_map(const RootMap& pmap) const {
      typedef bgl_named_params<RootMap, vertex_root_t, self> 
        Params;
      return Params(pmap, *this);
    }

    template <typename Vertex>
    bgl_named_params<Vertex, root_vertex_t, self>
    root_vertex(const Vertex& r) const {
      typedef bgl_named_params<Vertex, root_vertex_t, self> Params;
      return Params(r, *this);
    }

    template <typename EdgeCentralityMap>
    bgl_named_params<EdgeCentralityMap, edge_centrality_t, self>
    edge_centrality_map(const EdgeCentralityMap& r) const {
      typedef bgl_named_params<EdgeCentralityMap, edge_centrality_t, self> Params;
      return Params(r, *this);
    }

    template <typename CentralityMap>
    bgl_named_params<CentralityMap, vertex_centrality_t, self>
    centrality_map(const CentralityMap& r) const {
      typedef bgl_named_params<CentralityMap, vertex_centrality_t, self> Params;
      return Params(r, *this);
    }

    template <typename ColorMap>
    bgl_named_params<ColorMap, vertex_color_t, self>
    color_map(const ColorMap& pmap) const {
      typedef bgl_named_params<ColorMap, vertex_color_t, self> Params;
      return Params(pmap, *this);
    }

    template <typename ColorMap>
    bgl_named_params<ColorMap, vertex_color_t, self>
    vertex_color_map(const ColorMap& pmap) const {
      typedef bgl_named_params<ColorMap, vertex_color_t, self> Params;
      return Params(pmap, *this);
    }

    template <typename ColorMap>
    bgl_named_params<ColorMap, edge_color_t, self>
    edge_color_map(const ColorMap& pmap) const {
      typedef bgl_named_params<ColorMap, edge_color_t, self> Params;
      return Params(pmap, *this);
    }

    template <typename CapacityMap>
    bgl_named_params<CapacityMap, edge_capacity_t, self>
    capacity_map(CapacityMap pmap) {
      typedef bgl_named_params<CapacityMap, edge_capacity_t, self> Params;
      return Params(pmap, *this);
    }

    template <typename Residual_CapacityMap>
    bgl_named_params<Residual_CapacityMap, edge_residual_capacity_t, self>
    residual_capacity_map(Residual_CapacityMap pmap) {
      typedef bgl_named_params<Residual_CapacityMap, 
        edge_residual_capacity_t, self>
        Params;
      return Params(pmap, *this);
    }

    template <typename ReverseMap>
    bgl_named_params<ReverseMap, edge_reverse_t, self>
    reverse_edge_map(ReverseMap pmap) {
      typedef bgl_named_params<ReverseMap, 
        edge_reverse_t, self>
        Params;
      return Params(pmap, *this);
    }

    template <typename DiscoverTimeMap>
    bgl_named_params<DiscoverTimeMap, vertex_discover_time_t, self>
    discover_time_map(const DiscoverTimeMap& pmap) const {
      typedef bgl_named_params<DiscoverTimeMap, vertex_discover_time_t, self>
        Params;
      return Params(pmap, *this);
    }

    template <typename LowPointMap>
    bgl_named_params<LowPointMap, vertex_lowpoint_t, self>
    lowpoint_map(const LowPointMap& pmap) const {
      typedef bgl_named_params<LowPointMap, vertex_lowpoint_t, self>
        Params;
      return Params(pmap, *this);
    }

    template <typename IndexMap>
    bgl_named_params<IndexMap, vertex_index_t, self>
    vertex_index_map(const IndexMap& pmap) const {
      typedef bgl_named_params<IndexMap, vertex_index_t, self> Params;
      return Params(pmap, *this);
    }

    template <typename IndexMap>
    bgl_named_params<IndexMap, vertex_index1_t, self>
    vertex_index1_map(const IndexMap& pmap) const {
      typedef bgl_named_params<IndexMap, vertex_index1_t, self> Params;
      return Params(pmap, *this);
    }

    template <typename IndexMap>
    bgl_named_params<IndexMap, vertex_index2_t, self>
    vertex_index2_map(const IndexMap& pmap) const {
      typedef bgl_named_params<IndexMap, vertex_index2_t, self> Params;
      return Params(pmap, *this);
    }

    template <typename Visitor>
    bgl_named_params<Visitor, graph_visitor_t, self>
    visitor(const Visitor& vis) const {
      typedef bgl_named_params<Visitor, graph_visitor_t, self> Params;
      return Params(vis, *this);
    }

    template <typename Compare>
    bgl_named_params<Compare, distance_compare_t, self>
    distance_compare(Compare cmp) const {
      typedef bgl_named_params<Compare, distance_compare_t, self> Params;
      return Params(cmp, *this);
    }

    template <typename Combine>
    bgl_named_params<Combine, distance_combine_t, self>
    distance_combine(Combine cmb) const {
      typedef bgl_named_params<Combine, distance_combine_t, self> Params;
      return Params(cmb, *this);
    }

    template <typename Init>
    bgl_named_params<Init, distance_inf_t, self>
    distance_inf(Init init) const {
      typedef bgl_named_params<Init, distance_inf_t, self> Params;
      return Params(init, *this);
    }

    template <typename Init>
    bgl_named_params<Init, distance_zero_t, self>
    distance_zero(Init init) const {
      typedef bgl_named_params<Init, distance_zero_t, self> Params;
      return Params(init, *this);
    }

    template <typename Buffer>
    bgl_named_params<detail::wrap_ref<Buffer>, buffer_param_t, self>
    buffer(Buffer& b) const {
      typedef bgl_named_params<detail::wrap_ref<Buffer>, buffer_param_t, self> 
        Params;
      return Params(detail::wrap_ref<Buffer>(b), *this);
    }

    template <typename Copier>
    bgl_named_params<Copier, edge_copy_t, self>
    edge_copy(const Copier& c) const {
      typedef bgl_named_params<Copier, edge_copy_t, self> Params;
      return Params(c, *this);
    }

    template <typename Copier>
    bgl_named_params<Copier, vertex_copy_t, self>
    vertex_copy(const Copier& c) const {
      typedef bgl_named_params<Copier, vertex_copy_t, self> Params;
      return Params(c, *this);
    }

    template <typename Orig2CopyMap>
    bgl_named_params<Orig2CopyMap, orig_to_copy_t, self>
    orig_to_copy(const Orig2CopyMap& c) const {
      typedef bgl_named_params<Orig2CopyMap, orig_to_copy_t, self> Params;
      return Params(c, *this);
    }

    template <typename IsoMap>
    bgl_named_params<IsoMap, vertex_isomorphism_t, self>
    isomorphism_map(const IsoMap& c) const {
      typedef bgl_named_params<IsoMap, vertex_isomorphism_t, self> Params;
      return Params(c, *this);
    }

    template <typename VertexInvar>
    bgl_named_params<VertexInvar, vertex_invariant_t, self>
    vertex_invariant(const VertexInvar& c) const {
      typedef bgl_named_params<VertexInvar, vertex_invariant_t, self> Params;
      return Params(c, *this);
    }

    template <typename VertexDisplacement>
    bgl_named_params<VertexDisplacement, vertex_displacement_t, self>
    displacement_map(const VertexDisplacement& c) const {
      typedef bgl_named_params<VertexDisplacement, vertex_displacement_t, self> Params;
      return Params(c, *this);
    }

    template <typename AttractiveForce>
    bgl_named_params<AttractiveForce, attractive_force_t, self>
    attractive_force(const AttractiveForce& c) {
      typedef bgl_named_params<AttractiveForce, attractive_force_t, self> Params;
      return Params(c, *this);
    }
    
    template <typename RepulsiveForce>
    bgl_named_params<RepulsiveForce, repulsive_force_t, self>
    repulsive_force(const RepulsiveForce& c) {
      typedef bgl_named_params<RepulsiveForce, repulsive_force_t, self> Params;
      return Params(c, *this);
    }
    
    template <typename ForcePairs>
    bgl_named_params<ForcePairs, force_pairs_t, self>
    force_pairs(const ForcePairs& c) {
      typedef bgl_named_params<ForcePairs, force_pairs_t, self> Params;
      return Params(c, *this);
    }

    template <typename Cooling>
    bgl_named_params<Cooling, cooling_t, self>
    cooling(const Cooling& c) {
      typedef bgl_named_params<Cooling, cooling_t, self> Params;
      return Params(c, *this);
    }

    template <typename TP>
    bgl_named_params<TP, iterations_t, self>
    iterations(const TP& c) {
      typedef bgl_named_params<TP, iterations_t, self> Params;
      return Params(c, *this);
    }    

    template<typename TP>
    bgl_named_params<std::pair<TP, TP>, diameter_range_t, self>
    diameter_range(const std::pair<TP, TP>& c) {
      typedef bgl_named_params<std::pair<TP, TP>, diameter_range_t, self> Params;
      return Params(c, *this);
    }

    template<typename TP>
    bgl_named_params<std::pair<TP, TP>, learning_constant_range_t, self>
    learning_constant_range(const std::pair<TP, TP>& c) {
      typedef bgl_named_params<std::pair<TP, TP>, learning_constant_range_t, self>
        Params;
      return Params(c, *this);
    }
  };

  template <typename WeightMap>
  bgl_named_params<WeightMap, edge_weight_t>
  weight_map(WeightMap pmap) {
    typedef bgl_named_params<WeightMap, edge_weight_t> Params;
    return Params(pmap);
  }

  template <typename WeightMap>
  bgl_named_params<WeightMap, edge_weight2_t>
  weight_map2(WeightMap pmap) {
    typedef bgl_named_params<WeightMap, edge_weight2_t> Params;
    return Params(pmap);
  }

  template <typename DistanceMap>
  bgl_named_params<DistanceMap, vertex_distance_t>
  distance_map(DistanceMap pmap) {
    typedef bgl_named_params<DistanceMap, vertex_distance_t> Params;
    return Params(pmap);
  }

  template <typename PredecessorMap>
  bgl_named_params<PredecessorMap, vertex_predecessor_t>
  predecessor_map(PredecessorMap pmap) {
    typedef bgl_named_params<PredecessorMap, vertex_predecessor_t> Params;
    return Params(pmap);
  }

  template <typename RankMap>
  bgl_named_params<RankMap, vertex_rank_t>
  rank_map(RankMap pmap) {
    typedef bgl_named_params<RankMap, vertex_rank_t> Params;
    return Params(pmap);
  }

  template <typename RootMap>
  bgl_named_params<RootMap, vertex_root_t>
  root_map(RootMap pmap) {
    typedef bgl_named_params<RootMap, vertex_root_t> Params;
    return Params(pmap);
  }

  template <typename Vertex>
  bgl_named_params<Vertex, root_vertex_t>
  root_vertex(const Vertex& r) {
    typedef bgl_named_params<Vertex, root_vertex_t> Params;
    return Params(r);
  }

  template <typename EdgeCentralityMap>
  bgl_named_params<EdgeCentralityMap, edge_centrality_t>
  edge_centrality_map(const EdgeCentralityMap& r) {
    typedef bgl_named_params<EdgeCentralityMap, edge_centrality_t> Params;
    return Params(r);
  }

  template <typename CentralityMap>
  bgl_named_params<CentralityMap, vertex_centrality_t>
  centrality_map(const CentralityMap& r) {
    typedef bgl_named_params<CentralityMap, vertex_centrality_t> Params;
    return Params(r);
  }

  template <typename ColorMap>
  bgl_named_params<ColorMap, vertex_color_t>
  color_map(ColorMap pmap) {
    typedef bgl_named_params<ColorMap, vertex_color_t> Params;
    return Params(pmap);
  }

  template <typename CapacityMap>
  bgl_named_params<CapacityMap, edge_capacity_t>
  capacity_map(CapacityMap pmap) {
    typedef bgl_named_params<CapacityMap, edge_capacity_t> Params;
    return Params(pmap);
  }

  template <typename Residual_CapacityMap>
  bgl_named_params<Residual_CapacityMap, edge_residual_capacity_t>
  residual_capacity_map(Residual_CapacityMap pmap) {
    typedef bgl_named_params<Residual_CapacityMap, edge_residual_capacity_t>
      Params;
    return Params(pmap);
  }

  template <typename ReverseMap>
  bgl_named_params<ReverseMap, edge_reverse_t>
  reverse_edge_map(ReverseMap pmap) {
    typedef bgl_named_params<ReverseMap, edge_reverse_t>
      Params;
    return Params(pmap);
  }

  template <typename DiscoverTimeMap>
  bgl_named_params<DiscoverTimeMap, vertex_discover_time_t>
  discover_time_map(DiscoverTimeMap pmap) {
    typedef bgl_named_params<DiscoverTimeMap, vertex_discover_time_t> Params;
    return Params(pmap);
  }

  template <typename LowPointMap>
  bgl_named_params<LowPointMap, vertex_lowpoint_t>
  lowpoint_map(LowPointMap pmap) {
    typedef bgl_named_params<LowPointMap, vertex_lowpoint_t> Params;
    return Params(pmap);
  }

  template <typename IndexMap>
  bgl_named_params<IndexMap, vertex_index_t>
  vertex_index_map(IndexMap pmap) {
    typedef bgl_named_params<IndexMap, vertex_index_t> Params;
    return Params(pmap);
  }

  template <typename IndexMap>
  bgl_named_params<IndexMap, vertex_index1_t>
  vertex_index1_map(const IndexMap& pmap) {
    typedef bgl_named_params<IndexMap, vertex_index1_t> Params;
    return Params(pmap);
  }

  template <typename IndexMap>
  bgl_named_params<IndexMap, vertex_index2_t>
  vertex_index2_map(const IndexMap& pmap) {
    typedef bgl_named_params<IndexMap, vertex_index2_t> Params;
    return Params(pmap);
  }

  template <typename Visitor>
  bgl_named_params<Visitor, graph_visitor_t>
  visitor(const Visitor& vis) {
    typedef bgl_named_params<Visitor, graph_visitor_t> Params;
    return Params(vis);
  }

  template <typename Compare>
  bgl_named_params<Compare, distance_compare_t>
  distance_compare(Compare cmp) {
    typedef bgl_named_params<Compare, distance_compare_t> Params;
    return Params(cmp);
  }

  template <typename Combine>
  bgl_named_params<Combine, distance_combine_t>
  distance_combine(Combine cmb) {
    typedef bgl_named_params<Combine, distance_combine_t> Params;
    return Params(cmb);
  }

  template <typename Init>
  bgl_named_params<Init, distance_inf_t>
  distance_inf(Init init) {
    typedef bgl_named_params<Init, distance_inf_t> Params;
    return Params(init);
  }

  template <typename Init>
  bgl_named_params<Init, distance_zero_t>
  distance_zero(Init init) {
    typedef bgl_named_params<Init, distance_zero_t> Params;
    return Params(init);
  }

  template <typename Buffer>
  bgl_named_params<detail::wrap_ref<Buffer>, buffer_param_t>
  buffer(Buffer& b) {
    typedef bgl_named_params<detail::wrap_ref<Buffer>, buffer_param_t> Params;
    return Params(detail::wrap_ref<Buffer>(b));
  }

  template <typename Copier>
  bgl_named_params<Copier, edge_copy_t>
  edge_copy(const Copier& c) {
    typedef bgl_named_params<Copier, edge_copy_t> Params;
    return Params(c);
  }

  template <typename Copier>
  bgl_named_params<Copier, vertex_copy_t>
  vertex_copy(const Copier& c) {
    typedef bgl_named_params<Copier, vertex_copy_t> Params;
    return Params(c);
  }

  template <typename Orig2CopyMap>
  bgl_named_params<Orig2CopyMap, orig_to_copy_t>
  orig_to_copy(const Orig2CopyMap& c) {
    typedef bgl_named_params<Orig2CopyMap, orig_to_copy_t> Params;
    return Params(c);
  }

  template <typename IsoMap>
  bgl_named_params<IsoMap, vertex_isomorphism_t>
  isomorphism_map(const IsoMap& c) {
    typedef bgl_named_params<IsoMap, vertex_isomorphism_t> Params;
    return Params(c);
  }

  template <typename VertexInvar>
  bgl_named_params<VertexInvar, vertex_invariant_t>
  vertex_invariant(const VertexInvar& c) {
    typedef bgl_named_params<VertexInvar, vertex_invariant_t> Params;
    return Params(c);
  }

  template <typename VertexDisplacement>
  bgl_named_params<VertexDisplacement, vertex_displacement_t>
  displacement_map(const VertexDisplacement& c) {
    typedef bgl_named_params<VertexDisplacement, vertex_displacement_t> Params;
    return Params(c);
  }

  template <typename AttractiveForce>
  bgl_named_params<AttractiveForce, attractive_force_t>
  attractive_force(const AttractiveForce& c) {
    typedef bgl_named_params<AttractiveForce, attractive_force_t> Params;
    return Params(c);
  }

  template <typename RepulsiveForce>
  bgl_named_params<RepulsiveForce, repulsive_force_t>
  repulsive_force(const RepulsiveForce& c) {
    typedef bgl_named_params<RepulsiveForce, repulsive_force_t> Params;
    return Params(c);
  }

  template <typename ForcePairs>
  bgl_named_params<ForcePairs, force_pairs_t>
  force_pairs(const ForcePairs& c) {
    typedef bgl_named_params<ForcePairs, force_pairs_t> Params;
    return Params(c);
  }

  template <typename Cooling>
  bgl_named_params<Cooling, cooling_t>
  cooling(const Cooling& c) {
    typedef bgl_named_params<Cooling, cooling_t> Params;
    return Params(c);
  }

  template <typename T>
  bgl_named_params<T, iterations_t>
  iterations(const T& c) {
    typedef bgl_named_params<T, iterations_t> Params;
    return Params(c);
  }    
  
  template<typename T>
  bgl_named_params<std::pair<T, T>, diameter_range_t>
  diameter_range(const std::pair<T, T>& c) {
    typedef bgl_named_params<std::pair<T, T>, diameter_range_t> Params;
    return Params(c);
  }
  
  template<typename T>
  bgl_named_params<std::pair<T, T>, learning_constant_range_t>
  learning_constant_range(const std::pair<T, T>& c) {
    typedef bgl_named_params<std::pair<T, T>, learning_constant_range_t>
      Params;
    return Params(c);
  }

  //===========================================================================
  // Functions for extracting parameters from bgl_named_params

  template <class Tag1, class Tag2, class T1, class Base>
  inline
  typename property_value< bgl_named_params<T1,Tag1,Base>, Tag2>::type
  get_param(const bgl_named_params<T1,Tag1,Base>& p, Tag2 tag2)
  {
    enum { match = detail::same_property<Tag1,Tag2>::value };
    typedef typename
      property_value< bgl_named_params<T1,Tag1,Base>, Tag2>::type T2;
    T2* t2 = 0;
    typedef detail::property_value_dispatch<match> Dispatcher;
    return Dispatcher::const_get_value(p, t2, tag2);
  }


  namespace detail {
    // MSVC++ workaround
    template <class Param>
    struct choose_param_helper {
      template <class Default> struct result { typedef Param type; };
      template <typename Default>
      static const Param& apply(const Param& p, const Default&) { return p; }
    };
    template <>
    struct choose_param_helper<error_property_not_found> {
      template <class Default> struct result { typedef Default type; };
      template <typename Default>
      static const Default& apply(const error_property_not_found&, const Default& d)
        { return d; }
    };
  } // namespace detail

  template <class P, class Default> 
  const typename detail::choose_param_helper<P>::template result<Default>::type&
  choose_param(const P& param, const Default& d) { 
    return detail::choose_param_helper<P>::apply(param, d);
  }

  template <typename T>
  inline bool is_default_param(const T&) { return false; }

  inline bool is_default_param(const detail::error_property_not_found&)
    { return true; }

  namespace detail {

    struct choose_parameter {
      template <class P, class Graph, class Tag>
      struct bind_ {
        typedef const P& const_result_type;
        typedef const P& result_type;
        typedef P type;
      };

      template <class P, class Graph, class Tag>
      static typename bind_<P, Graph, Tag>::const_result_type
      const_apply(const P& p, const Graph&, Tag&) 
      { return p; }

      template <class P, class Graph, class Tag>
      static typename bind_<P, Graph, Tag>::result_type
      apply(const P& p, Graph&, Tag&) 
      { return p; }
    };

    struct choose_default_param {
      template <class P, class Graph, class Tag>
      struct bind_ {
        typedef typename property_map<Graph, Tag>::type 
          result_type;
        typedef typename property_map<Graph, Tag>::const_type 
          const_result_type;
        typedef typename property_map<Graph, Tag>::const_type 
          type;
      };

      template <class P, class Graph, class Tag>
      static typename bind_<P, Graph, Tag>::const_result_type
      const_apply(const P&, const Graph& g, Tag tag) { 
        return get(tag, g); 
      }
      template <class P, class Graph, class Tag>
      static typename bind_<P, Graph, Tag>::result_type
      apply(const P&, Graph& g, Tag tag) { 
        return get(tag, g); 
      }
    };

    template <class Param>
    struct choose_property_map {
      typedef choose_parameter type;
    };
    template <>
    struct choose_property_map<detail::error_property_not_found> {
      typedef choose_default_param type;
    };

    template <class Param, class Graph, class Tag>
    struct choose_pmap_helper {
      typedef typename choose_property_map<Param>::type Selector;
      typedef typename Selector:: template bind_<Param, Graph, Tag> Bind;
      typedef Bind type;
      typedef typename Bind::result_type result_type;
      typedef typename Bind::const_result_type const_result_type;
      typedef typename Bind::type result;
    };

    // used in the max-flow algorithms
    template <class Graph, class P, class T, class R>
    struct edge_capacity_value
    {
      typedef bgl_named_params<P, T, R> Params;
      typedef typename property_value< Params, edge_capacity_t>::type Param;
      typedef typename detail::choose_pmap_helper<Param, Graph,
        edge_capacity_t>::result CapacityEdgeMap;
      typedef typename property_traits<CapacityEdgeMap>::value_type type;
    };

  } // namespace detail
  

  // Use this function instead of choose_param() when you want
  // to avoid requiring get(tag, g) when it is not used. 
  template <typename Param, typename Graph, typename PropertyTag>
  typename
    detail::choose_pmap_helper<Param,Graph,PropertyTag>::const_result_type
  choose_const_pmap(const Param& p, const Graph& g, PropertyTag tag)
  { 
    typedef typename 
      detail::choose_pmap_helper<Param,Graph,PropertyTag>::Selector Choice;
    return Choice::const_apply(p, g, tag);
  }

  template <typename Param, typename Graph, typename PropertyTag>
  typename detail::choose_pmap_helper<Param,Graph,PropertyTag>::result_type
  choose_pmap(const Param& p, Graph& g, PropertyTag tag)
  { 
    typedef typename 
      detail::choose_pmap_helper<Param,Graph,PropertyTag>::Selector Choice;
    return Choice::apply(p, g, tag);
  }

} // namespace boost

#endif // BOOST_GRAPH_NAMED_FUNCTION_PARAMS_HPP
