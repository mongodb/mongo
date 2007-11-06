// Copyright (C) 2001 Jeremy Siek, Douglas Gregor, Brian Osman
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_GRAPH_ISOMORPHISM_HPP
#define BOOST_GRAPH_ISOMORPHISM_HPP

#include <utility>
#include <vector>
#include <iterator>
#include <algorithm>
#include <boost/config.hpp>
#include <boost/graph/depth_first_search.hpp>
#include <boost/utility.hpp>
#include <boost/detail/algorithm.hpp>
#include <boost/pending/indirect_cmp.hpp> // for make_indirect_pmap

#ifndef BOOST_GRAPH_ITERATION_MACROS_HPP
#define BOOST_ISO_INCLUDED_ITER_MACROS // local macro, see bottom of file
#include <boost/graph/iteration_macros.hpp>
#endif

namespace boost {

  namespace detail {

    template <typename Graph1, typename Graph2, typename IsoMapping,
      typename Invariant1, typename Invariant2,
      typename IndexMap1, typename IndexMap2>
    class isomorphism_algo
    {
      typedef typename graph_traits<Graph1>::vertex_descriptor vertex1_t;
      typedef typename graph_traits<Graph2>::vertex_descriptor vertex2_t;
      typedef typename graph_traits<Graph1>::edge_descriptor edge1_t;
      typedef typename graph_traits<Graph1>::vertices_size_type size_type;
      typedef typename Invariant1::result_type invar1_value;
      typedef typename Invariant2::result_type invar2_value;
    
      const Graph1& G1;
      const Graph2& G2;
      IsoMapping f;
      Invariant1 invariant1;
      Invariant2 invariant2;
      std::size_t max_invariant;
      IndexMap1 index_map1;
      IndexMap2 index_map2;
    
      std::vector<vertex1_t> dfs_vertices;
      typedef typename std::vector<vertex1_t>::iterator vertex_iter;
      std::vector<int> dfs_num_vec;
      typedef safe_iterator_property_map<typename std::vector<int>::iterator,
                                         IndexMap1
#ifdef BOOST_NO_STD_ITERATOR_TRAITS
                                         , int, int&
#endif /* BOOST_NO_STD_ITERATOR_TRAITS */
                                         > DFSNumMap;
      DFSNumMap dfs_num;
      std::vector<edge1_t> ordered_edges;
      typedef typename std::vector<edge1_t>::iterator edge_iter;
    
      std::vector<char> in_S_vec;
      typedef safe_iterator_property_map<typename std::vector<char>::iterator,
                                         IndexMap2
#ifdef BOOST_NO_STD_ITERATOR_TRAITS
                                         , char, char&
#endif /* BOOST_NO_STD_ITERATOR_TRAITS */
                                         > InSMap;
      InSMap in_S;
    
      int num_edges_on_k;
    
      friend struct compare_multiplicity;
      struct compare_multiplicity
      {
        compare_multiplicity(Invariant1 invariant1, size_type* multiplicity)
          : invariant1(invariant1), multiplicity(multiplicity) { }
        bool operator()(const vertex1_t& x, const vertex1_t& y) const {
          return multiplicity[invariant1(x)] < multiplicity[invariant1(y)];
        }
        Invariant1 invariant1;
        size_type* multiplicity;
      };
    
      struct record_dfs_order : default_dfs_visitor
      {
        record_dfs_order(std::vector<vertex1_t>& v, std::vector<edge1_t>& e) 
          : vertices(v), edges(e) { }
    
        void discover_vertex(vertex1_t v, const Graph1&) const {
          vertices.push_back(v);
        }
        void examine_edge(edge1_t e, const Graph1& G1) const {
          edges.push_back(e);
        }
        std::vector<vertex1_t>& vertices;
        std::vector<edge1_t>& edges;
      };
    
      struct edge_cmp {
        edge_cmp(const Graph1& G1, DFSNumMap dfs_num)
          : G1(G1), dfs_num(dfs_num) { }
        bool operator()(const edge1_t& e1, const edge1_t& e2) const {
          using namespace std;
          int u1 = dfs_num[source(e1,G1)], v1 = dfs_num[target(e1,G1)];
          int u2 = dfs_num[source(e2,G1)], v2 = dfs_num[target(e2,G1)];
          int m1 = (max)(u1, v1);
          int m2 = (max)(u2, v2);
          // lexicographical comparison 
          return std::make_pair(m1, std::make_pair(u1, v1))
            < std::make_pair(m2, std::make_pair(u2, v2));
        }
        const Graph1& G1;
        DFSNumMap dfs_num;
      };
    
    public:
      isomorphism_algo(const Graph1& G1, const Graph2& G2, IsoMapping f,
                       Invariant1 invariant1, Invariant2 invariant2, std::size_t max_invariant,
                       IndexMap1 index_map1, IndexMap2 index_map2)
        : G1(G1), G2(G2), f(f), invariant1(invariant1), invariant2(invariant2),
          max_invariant(max_invariant),
          index_map1(index_map1), index_map2(index_map2)
      {
        in_S_vec.resize(num_vertices(G1));
        in_S = make_safe_iterator_property_map
          (in_S_vec.begin(), in_S_vec.size(), index_map2
#ifdef BOOST_NO_STD_ITERATOR_TRAITS
           , in_S_vec.front()
#endif /* BOOST_NO_STD_ITERATOR_TRAITS */
           );
      }
    
      bool test_isomorphism()
      {
        {
          std::vector<invar1_value> invar1_array;
          BGL_FORALL_VERTICES_T(v, G1, Graph1)
            invar1_array.push_back(invariant1(v));
          sort(invar1_array);
        
          std::vector<invar2_value> invar2_array;
          BGL_FORALL_VERTICES_T(v, G2, Graph2)
            invar2_array.push_back(invariant2(v));
          sort(invar2_array);
          if (! equal(invar1_array, invar2_array))
            return false;
        }
        
        std::vector<vertex1_t> V_mult;
        BGL_FORALL_VERTICES_T(v, G1, Graph1)
          V_mult.push_back(v);
        {
          std::vector<size_type> multiplicity(max_invariant, 0);
          BGL_FORALL_VERTICES_T(v, G1, Graph1)
            ++multiplicity[invariant1(v)];
          sort(V_mult, compare_multiplicity(invariant1, &multiplicity[0]));
        }
        
        std::vector<default_color_type> color_vec(num_vertices(G1));
        safe_iterator_property_map<std::vector<default_color_type>::iterator,
                                   IndexMap1
#ifdef BOOST_NO_STD_ITERATOR_TRAITS
                                   , default_color_type, default_color_type&
#endif /* BOOST_NO_STD_ITERATOR_TRAITS */
                                   >
          color_map(color_vec.begin(), color_vec.size(), index_map1);
        record_dfs_order dfs_visitor(dfs_vertices, ordered_edges);
        typedef color_traits<default_color_type> Color;
        for (vertex_iter u = V_mult.begin(); u != V_mult.end(); ++u) {
          if (color_map[*u] == Color::white()) {
            dfs_visitor.start_vertex(*u, G1);
            depth_first_visit(G1, *u, dfs_visitor, color_map);
          }
        }
        // Create the dfs_num array and dfs_num_map
        dfs_num_vec.resize(num_vertices(G1));
        dfs_num = make_safe_iterator_property_map(dfs_num_vec.begin(),
                                                  dfs_num_vec.size(), 
                                                  index_map1
#ifdef BOOST_NO_STD_ITERATOR_TRAITS
                                                  , dfs_num_vec.front()
#endif /* BOOST_NO_STD_ITERATOR_TRAITS */
                                                  );
        size_type n = 0;
        for (vertex_iter v = dfs_vertices.begin(); v != dfs_vertices.end(); ++v)
          dfs_num[*v] = n++;
        
        sort(ordered_edges, edge_cmp(G1, dfs_num));
        
    
        int dfs_num_k = -1;
        return this->match(ordered_edges.begin(), dfs_num_k);
      }
    
    private:
      bool match(edge_iter iter, int dfs_num_k)
      {
        if (iter != ordered_edges.end()) {
          vertex1_t i = source(*iter, G1), j = target(*iter, G2);
          if (dfs_num[i] > dfs_num_k) {
            vertex1_t kp1 = dfs_vertices[dfs_num_k + 1];
            BGL_FORALL_VERTICES_T(u, G2, Graph2) {
              if (invariant1(kp1) == invariant2(u) && in_S[u] == false) {
                f[kp1] = u;
                in_S[u] = true;
                num_edges_on_k = 0;
                
                if (match(iter, dfs_num_k + 1))
#if 0
                    // dwa 2003/7/11 -- this *HAS* to be a bug!
                    ;
#endif 
                    return true;
                    
                in_S[u] = false;
              }
            }
               
          }
          else if (dfs_num[j] > dfs_num_k) {
            vertex1_t k = dfs_vertices[dfs_num_k];
            num_edges_on_k -= 
              count_if(adjacent_vertices(f[k], G2), make_indirect_pmap(in_S));
                
            for (int jj = 0; jj < dfs_num_k; ++jj) {
              vertex1_t j = dfs_vertices[jj];
              num_edges_on_k -= count(adjacent_vertices(f[j], G2), f[k]);
            }
                
            if (num_edges_on_k != 0)
              return false;
            BGL_FORALL_ADJ_T(f[i], v, G2, Graph2)
              if (invariant2(v) == invariant1(j) && in_S[v] == false) {
                f[j] = v;
                in_S[v] = true;
                num_edges_on_k = 1;
                BOOST_USING_STD_MAX();
                int next_k = max BOOST_PREVENT_MACRO_SUBSTITUTION(dfs_num_k, max BOOST_PREVENT_MACRO_SUBSTITUTION(dfs_num[i], dfs_num[j]));
                if (match(next(iter), next_k))
                  return true;
                in_S[v] = false;
              }
                
                
          }
          else {
            if (contains(adjacent_vertices(f[i], G2), f[j])) {
              ++num_edges_on_k;
              if (match(next(iter), dfs_num_k))
                return true;
            }
                
          }
        } else 
          return true;
        return false;
      }
    
    };

    
    template <typename Graph, typename InDegreeMap>
    void compute_in_degree(const Graph& g, InDegreeMap in_degree_map)
    {
      BGL_FORALL_VERTICES_T(v, g, Graph)
        put(in_degree_map, v, 0);

      BGL_FORALL_VERTICES_T(u, g, Graph)
        BGL_FORALL_ADJ_T(u, v, g, Graph)
        put(in_degree_map, v, get(in_degree_map, v) + 1);
    }

  } // namespace detail


  template <typename InDegreeMap, typename Graph>
  class degree_vertex_invariant
  {
    typedef typename graph_traits<Graph>::vertex_descriptor vertex_t;
    typedef typename graph_traits<Graph>::degree_size_type size_type;
  public:
    typedef vertex_t argument_type;
    typedef size_type result_type;

    degree_vertex_invariant(const InDegreeMap& in_degree_map, const Graph& g)
      : m_in_degree_map(in_degree_map), m_g(g) { }

    size_type operator()(vertex_t v) const {
      return (num_vertices(m_g) + 1) * out_degree(v, m_g)
        + get(m_in_degree_map, v);
    }
    // The largest possible vertex invariant number
    size_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { 
      return num_vertices(m_g) * num_vertices(m_g) + num_vertices(m_g);
    }
  private:
    InDegreeMap m_in_degree_map;
    const Graph& m_g;
  };


  template <typename Graph1, typename Graph2, typename IsoMapping, 
    typename Invariant1, typename Invariant2,
    typename IndexMap1, typename IndexMap2>
  bool isomorphism(const Graph1& G1, const Graph2& G2, IsoMapping f, 
                   Invariant1 invariant1, Invariant2 invariant2, 
                   std::size_t max_invariant,
                   IndexMap1 index_map1, IndexMap2 index_map2)

  {
    // Graph requirements
    function_requires< VertexListGraphConcept<Graph1> >();
    function_requires< EdgeListGraphConcept<Graph1> >();
    function_requires< VertexListGraphConcept<Graph2> >();
    function_requires< BidirectionalGraphConcept<Graph2> >();
    
    typedef typename graph_traits<Graph1>::vertex_descriptor vertex1_t;
    typedef typename graph_traits<Graph2>::vertex_descriptor vertex2_t;
    typedef typename graph_traits<Graph1>::vertices_size_type size_type;
    
    // Vertex invariant requirement
    function_requires< AdaptableUnaryFunctionConcept<Invariant1,
      size_type, vertex1_t> >();
    function_requires< AdaptableUnaryFunctionConcept<Invariant2,
      size_type, vertex2_t> >();
    
    // Property map requirements
    function_requires< ReadWritePropertyMapConcept<IsoMapping, vertex1_t> >();
    typedef typename property_traits<IsoMapping>::value_type IsoMappingValue;
    BOOST_STATIC_ASSERT((is_same<IsoMappingValue, vertex2_t>::value));
    
    function_requires< ReadablePropertyMapConcept<IndexMap1, vertex1_t> >();
    typedef typename property_traits<IndexMap1>::value_type IndexMap1Value;
    BOOST_STATIC_ASSERT((is_convertible<IndexMap1Value, size_type>::value));
    
    function_requires< ReadablePropertyMapConcept<IndexMap2, vertex2_t> >();
    typedef typename property_traits<IndexMap2>::value_type IndexMap2Value;
    BOOST_STATIC_ASSERT((is_convertible<IndexMap2Value, size_type>::value));
    
    if (num_vertices(G1) != num_vertices(G2))
      return false;
    if (num_vertices(G1) == 0 && num_vertices(G2) == 0)
      return true;
    
    detail::isomorphism_algo<Graph1, Graph2, IsoMapping, Invariant1,
      Invariant2, IndexMap1, IndexMap2> 
      algo(G1, G2, f, invariant1, invariant2, max_invariant, 
           index_map1, index_map2);
    return algo.test_isomorphism();
  }


  namespace detail {
  
    template <typename Graph1, typename Graph2, 
      typename IsoMapping, 
      typename IndexMap1, typename IndexMap2,
      typename P, typename T, typename R>
    bool isomorphism_impl(const Graph1& G1, const Graph2& G2, 
                          IsoMapping f, IndexMap1 index_map1, IndexMap2 index_map2,
                          const bgl_named_params<P,T,R>& params)
    {
      std::vector<std::size_t> in_degree1_vec(num_vertices(G1));
      typedef safe_iterator_property_map<std::vector<std::size_t>::iterator,
                                         IndexMap1
#ifdef BOOST_NO_STD_ITERATOR_TRAITS
                                         , std::size_t, std::size_t&
#endif /* BOOST_NO_STD_ITERATOR_TRAITS */
                                         > InDeg1;
      InDeg1 in_degree1(in_degree1_vec.begin(), in_degree1_vec.size(), index_map1);
      compute_in_degree(G1, in_degree1);

      std::vector<std::size_t> in_degree2_vec(num_vertices(G2));
      typedef safe_iterator_property_map<std::vector<std::size_t>::iterator, 
                                         IndexMap2
#ifdef BOOST_NO_STD_ITERATOR_TRAITS
                                         , std::size_t, std::size_t&
#endif /* BOOST_NO_STD_ITERATOR_TRAITS */
                                         > InDeg2;
      InDeg2 in_degree2(in_degree2_vec.begin(), in_degree2_vec.size(), index_map2);
      compute_in_degree(G2, in_degree2);

      degree_vertex_invariant<InDeg1, Graph1> invariant1(in_degree1, G1);
      degree_vertex_invariant<InDeg2, Graph2> invariant2(in_degree2, G2);

      return isomorphism(G1, G2, f,
                         choose_param(get_param(params, vertex_invariant1_t()), invariant1),
                         choose_param(get_param(params, vertex_invariant2_t()), invariant2),
                         choose_param(get_param(params, vertex_max_invariant_t()), (invariant2.max)()),
                         index_map1, index_map2
                         );  
    }  
   
  } // namespace detail


  // Named parameter interface
  template <typename Graph1, typename Graph2, class P, class T, class R>
  bool isomorphism(const Graph1& g1,
                   const Graph2& g2,
                   const bgl_named_params<P,T,R>& params)
  {
    typedef typename graph_traits<Graph2>::vertex_descriptor vertex2_t;
    typename std::vector<vertex2_t>::size_type n = num_vertices(g1);
    std::vector<vertex2_t> f(n);
    return detail::isomorphism_impl
      (g1, g2, 
       choose_param(get_param(params, vertex_isomorphism_t()),
                    make_safe_iterator_property_map(f.begin(), f.size(),
                                                    choose_const_pmap(get_param(params, vertex_index1),
                                                                      g1, vertex_index), vertex2_t())),
       choose_const_pmap(get_param(params, vertex_index1), g1, vertex_index),
       choose_const_pmap(get_param(params, vertex_index2), g2, vertex_index),
       params
       );
  }

  // All defaults interface
  template <typename Graph1, typename Graph2>
  bool isomorphism(const Graph1& g1, const Graph2& g2)
  {
    return isomorphism(g1, g2,
                       bgl_named_params<int, buffer_param_t>(0));// bogus named param
  }


  // Verify that the given mapping iso_map from the vertices of g1 to the
  // vertices of g2 describes an isomorphism.
  // Note: this could be made much faster by specializing based on the graph
  // concepts modeled, but since we're verifying an O(n^(lg n)) algorithm,
  // O(n^4) won't hurt us.
  template<typename Graph1, typename Graph2, typename IsoMap>
  inline bool verify_isomorphism(const Graph1& g1, const Graph2& g2, IsoMap iso_map)
  {
#if 0
    // problematic for filtered_graph!
    if (num_vertices(g1) != num_vertices(g2) || num_edges(g1) != num_edges(g2))
      return false;
#endif
  
    for (typename graph_traits<Graph1>::edge_iterator e1 = edges(g1).first;
         e1 != edges(g1).second; ++e1) {
      bool found_edge = false;
      for (typename graph_traits<Graph2>::edge_iterator e2 = edges(g2).first;
           e2 != edges(g2).second && !found_edge; ++e2) {
        if (source(*e2, g2) == get(iso_map, source(*e1, g1)) &&
            target(*e2, g2) == get(iso_map, target(*e1, g1))) {
          found_edge = true;
        }
      }
    
      if (!found_edge)
        return false;
    }
  
    return true;
  }

} // namespace boost

#ifdef BOOST_ISO_INCLUDED_ITER_MACROS
#undef BOOST_ISO_INCLUDED_ITER_MACROS
#include <boost/graph/iteration_macros_undef.hpp>
#endif

#endif // BOOST_GRAPH_ISOMORPHISM_HPP
