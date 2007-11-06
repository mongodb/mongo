//=======================================================================
// Copyright 2001 University of Notre Dame.
// Copyright 2006 Trustees of Indiana University
// Authors: Jeremy G. Siek and Douglas Gregor <dgregor@cs.indiana.edu>
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#ifndef BOOST_ADJACENCY_MATRIX_HPP
#define BOOST_ADJACENCY_MATRIX_HPP

#include <boost/config.hpp>
#include <vector>
#include <memory>
#include <cassert>
#include <boost/limits.hpp>
#include <boost/iterator.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/graph_selectors.hpp>
#include <boost/pending/ct_if.hpp>
#include <boost/graph/adjacency_iterator.hpp>
#include <boost/graph/detail/edge.hpp>
#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/iterator/filter_iterator.hpp>
#include <boost/pending/integer_range.hpp>
#include <boost/graph/properties.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/static_assert.hpp>
#include <boost/type_traits/ice.hpp>

namespace boost {
  
  namespace detail {

    template <class Directed, class Vertex>
    class matrix_edge_desc_impl : public edge_desc_impl<Directed,Vertex>
    {
      typedef edge_desc_impl<Directed,Vertex> Base;
    public:
      matrix_edge_desc_impl() { }
      matrix_edge_desc_impl(bool exists, Vertex s, Vertex d, 
                            const void* ep = 0)
        : Base(s, d, ep), m_exists(exists) { }
      bool exists() const { return m_exists; }
    private:
      bool m_exists;
    };

    struct does_edge_exist {
      template <class Edge>
      bool operator()(const Edge& e) const { return e.exists(); }
    };

    template <typename EdgeProperty>
    bool get_edge_exists(const std::pair<bool, EdgeProperty>& stored_edge, int) {
      return stored_edge.first;
    }
    template <typename EdgeProperty>
    void set_edge_exists(
        std::pair<bool, EdgeProperty>& stored_edge, 
        bool flag,
        int
        ) {
      stored_edge.first = flag;
    }

    template <typename EdgeProxy>
    bool get_edge_exists(const EdgeProxy& edge_proxy, ...) {
      return edge_proxy;
    }
    template <typename EdgeProxy>
    EdgeProxy& set_edge_exists(EdgeProxy& edge_proxy, bool flag, ...) {
      edge_proxy = flag;
      return edge_proxy; // just to avoid never used warning
    }



    template <typename EdgeProperty>
    const EdgeProperty&
    get_property(const std::pair<bool, EdgeProperty>& stored_edge) {
      return stored_edge.second;
    }
    template <typename EdgeProperty>
    EdgeProperty&
    get_property(std::pair<bool, EdgeProperty>& stored_edge) {
      return stored_edge.second;
    }

    template <typename StoredEdgeProperty, typename EdgeProperty>
    inline void
    set_property(std::pair<bool, StoredEdgeProperty>& stored_edge, 
                 const EdgeProperty& ep, int) {
      stored_edge.second = ep;
    }

    inline const no_property& get_property(const char&) {
      static no_property s_prop;
      return s_prop;
    }
    inline no_property& get_property(char&) {
      static no_property s_prop;
      return s_prop;
    }
    template <typename EdgeProxy, typename EdgeProperty>
    inline void
    set_property(EdgeProxy, const EdgeProperty&, ...) {}
    
    //=======================================================================
    // Directed Out Edge Iterator

    template <
        typename VertexDescriptor, typename MatrixIter
      , typename VerticesSizeType, typename EdgeDescriptor
    >
    struct dir_adj_matrix_out_edge_iter
      : iterator_adaptor<
            dir_adj_matrix_out_edge_iter<VertexDescriptor, MatrixIter,  VerticesSizeType, EdgeDescriptor>
          , MatrixIter
          , EdgeDescriptor
          , use_default
          , EdgeDescriptor
          , std::ptrdiff_t
        >
    {
        typedef iterator_adaptor<
            dir_adj_matrix_out_edge_iter<VertexDescriptor, MatrixIter,  VerticesSizeType, EdgeDescriptor>
          , MatrixIter
          , EdgeDescriptor
          , use_default
          , EdgeDescriptor
          , std::ptrdiff_t
        > super_t;
        
        dir_adj_matrix_out_edge_iter() { }
        
        dir_adj_matrix_out_edge_iter(
            const MatrixIter& i
          , const VertexDescriptor& src
          , const VerticesSizeType& n
           )
            : super_t(i), m_src(src), m_targ(0), m_n(n)
        { }

        void increment() {
            ++this->base_reference();
            ++m_targ;
        }
        
        inline EdgeDescriptor
        dereference() const 
        {
            return EdgeDescriptor(get_edge_exists(*this->base(), 0), m_src, m_targ, 
                                  &get_property(*this->base()));
        }
        VertexDescriptor m_src, m_targ;
        VerticesSizeType m_n;
    };

    //=======================================================================
    // Directed In Edge Iterator

    template <
        typename VertexDescriptor, typename MatrixIter
      , typename VerticesSizeType, typename EdgeDescriptor
    >
    struct dir_adj_matrix_in_edge_iter
      : iterator_adaptor<
            dir_adj_matrix_in_edge_iter<VertexDescriptor, MatrixIter,  VerticesSizeType, EdgeDescriptor>
          , MatrixIter
          , EdgeDescriptor
          , use_default
          , EdgeDescriptor
          , std::ptrdiff_t
        >
    {
        typedef iterator_adaptor<
            dir_adj_matrix_in_edge_iter<VertexDescriptor, MatrixIter,  VerticesSizeType, EdgeDescriptor>
          , MatrixIter
          , EdgeDescriptor
          , use_default
          , EdgeDescriptor
          , std::ptrdiff_t
        > super_t;
        
        dir_adj_matrix_in_edge_iter() { }
        
        dir_adj_matrix_in_edge_iter(
            const MatrixIter& i
          , const MatrixIter& last
          , const VertexDescriptor& tgt
          , const VerticesSizeType& n
           )
          : super_t(i), m_last(last), m_src(0), m_targ(tgt), m_n(n)
        { }

        void increment() {
          if (VerticesSizeType(m_last - this->base_reference()) >= m_n) {
            this->base_reference() += m_n;
            ++m_src;
          } else {
            this->base_reference() = m_last;
          }
        }
        
        inline EdgeDescriptor
        dereference() const 
        {
            return EdgeDescriptor(get_edge_exists(*this->base(), 0), m_src, m_targ, 
                                  &get_property(*this->base()));
        }
        MatrixIter m_last;
        VertexDescriptor m_src, m_targ;
        VerticesSizeType m_n;
    };

    //=======================================================================
    // Undirected Out Edge Iterator

    template <
        typename VertexDescriptor, typename MatrixIter
      , typename VerticesSizeType, typename EdgeDescriptor
    >
    struct undir_adj_matrix_out_edge_iter 
      : iterator_adaptor<
            undir_adj_matrix_out_edge_iter<VertexDescriptor, MatrixIter,  VerticesSizeType, EdgeDescriptor>
          , MatrixIter
          , EdgeDescriptor
          , use_default
          , EdgeDescriptor
          , std::ptrdiff_t
        >
    {
        typedef iterator_adaptor<
            undir_adj_matrix_out_edge_iter<VertexDescriptor, MatrixIter,  VerticesSizeType, EdgeDescriptor>
          , MatrixIter
          , EdgeDescriptor
          , use_default
          , EdgeDescriptor
          , std::ptrdiff_t
        > super_t;
        
        undir_adj_matrix_out_edge_iter() { }
        
        undir_adj_matrix_out_edge_iter(
            const MatrixIter& i
          , const VertexDescriptor& src
          , const VerticesSizeType& n
        )
          : super_t(i), m_src(src), m_inc(src), m_targ(0), m_n(n)
        {}

        void increment()
        {
            if (m_targ < m_src)     // first half
            {
                ++this->base_reference();
            }
            else if (m_targ < m_n - 1)
            {                  // second half
                ++m_inc;
                this->base_reference() += m_inc;
            }
            else
            {                  // past-the-end
                this->base_reference() += m_n - m_src;
            }
            ++m_targ;
        }
        
        inline EdgeDescriptor
        dereference() const 
        {
            return EdgeDescriptor(
                get_edge_exists(*this->base(), 0), m_src, m_targ
              , &get_property(*this->base())
            );
        }
        
        VertexDescriptor m_src, m_inc, m_targ;
        VerticesSizeType m_n;
    };

    //=======================================================================
    // Undirected In Edge Iterator

    template <
        typename VertexDescriptor, typename MatrixIter
      , typename VerticesSizeType, typename EdgeDescriptor
    >
    struct undir_adj_matrix_in_edge_iter 
      : iterator_adaptor<
            undir_adj_matrix_in_edge_iter<VertexDescriptor, MatrixIter,  VerticesSizeType, EdgeDescriptor>
          , MatrixIter
          , EdgeDescriptor
          , use_default
          , EdgeDescriptor
          , std::ptrdiff_t
        >
    {
        typedef iterator_adaptor<
            undir_adj_matrix_in_edge_iter<VertexDescriptor, MatrixIter,  VerticesSizeType, EdgeDescriptor>
          , MatrixIter
          , EdgeDescriptor
          , use_default
          , EdgeDescriptor
          , std::ptrdiff_t
        > super_t;
        
        undir_adj_matrix_in_edge_iter() { }
        
        undir_adj_matrix_in_edge_iter(
            const MatrixIter& i
          , const VertexDescriptor& src
          , const VerticesSizeType& n
        )
          : super_t(i), m_src(src), m_inc(src), m_targ(0), m_n(n)
        {}

        void increment()
        {
            if (m_targ < m_src)     // first half
            {
                ++this->base_reference();
            }
            else if (m_targ < m_n - 1)
            {                  // second half
                ++m_inc;
                this->base_reference() += m_inc;
            }
            else
            {                  // past-the-end
                this->base_reference() += m_n - m_src;
            }
            ++m_targ;
        }
        
        inline EdgeDescriptor
        dereference() const 
        {
            return EdgeDescriptor(
                     get_edge_exists(*this->base(), 0), m_targ, m_src
              , &get_property(*this->base())
            );
        }
        
        VertexDescriptor m_src, m_inc, m_targ;
        VerticesSizeType m_n;
    };

    //=======================================================================
    // Edge Iterator

    template <typename Directed, typename MatrixIter, 
              typename VerticesSizeType, typename EdgeDescriptor>
    struct adj_matrix_edge_iter
      : iterator_adaptor<
            adj_matrix_edge_iter<Directed, MatrixIter,  VerticesSizeType, EdgeDescriptor>
          , MatrixIter
          , EdgeDescriptor
          , use_default
          , EdgeDescriptor
          , std::ptrdiff_t
        >
    {
        typedef iterator_adaptor<
            adj_matrix_edge_iter<Directed, MatrixIter,  VerticesSizeType, EdgeDescriptor>
          , MatrixIter
          , EdgeDescriptor
          , use_default
          , EdgeDescriptor
          , std::ptrdiff_t
        > super_t;
        
        adj_matrix_edge_iter() { }
        
        adj_matrix_edge_iter(const MatrixIter& i, const MatrixIter& start, const VerticesSizeType& n) 
            : super_t(i), m_start(start), m_src(0), m_targ(0), m_n(n) { }

        void increment()
        {
            increment_dispatch(this->base_reference(), Directed());
        }
        
        void increment_dispatch(MatrixIter& i, directedS)
        {
            ++i;
            if (m_targ == m_n - 1)
            {
                m_targ = 0;
                ++m_src;
            }
            else
            {
                ++m_targ;
            }
        }
        
        void increment_dispatch(MatrixIter& i, undirectedS)
        {
            ++i;
            if (m_targ == m_src)
            {
                m_targ = 0;
                ++m_src;
            }
            else
            {
                ++m_targ;
            }
        }

        inline EdgeDescriptor
        dereference() const 
        {
            return EdgeDescriptor(
                get_edge_exists(
                    *this->base(), 0), m_src, m_targ, &get_property(*this->base())
            );
        }
      
        MatrixIter m_start;
        VerticesSizeType m_src, m_targ, m_n;
    };

  } // namespace detail

  //=========================================================================
  // Adjacency Matrix Traits
  template <typename Directed = directedS>
  class adjacency_matrix_traits {
    typedef typename Directed::is_directed_t is_directed;
  public:
    // The bidirectionalS tag is not allowed with the adjacency_matrix
    // graph type. Instead, use directedS, which also provides the
    // functionality required for a Bidirectional Graph (in_edges,
    // in_degree, etc.).
#if !defined(_MSC_VER) || _MSC_VER > 1300
    BOOST_STATIC_ASSERT(type_traits::ice_not<(is_same<Directed, bidirectionalS>::value)>::value);
#endif

    typedef typename boost::ct_if_t<is_directed,
                                    bidirectional_tag, undirected_tag>::type
      directed_category;
    
    typedef disallow_parallel_edge_tag edge_parallel_category;
    
    typedef std::size_t vertex_descriptor;

    typedef detail::matrix_edge_desc_impl<directed_category,
      vertex_descriptor> edge_descriptor;
  };

  struct adjacency_matrix_class_tag { };

  struct adj_matrix_traversal_tag :
    public virtual adjacency_matrix_tag,
    public virtual vertex_list_graph_tag,
    public virtual incidence_graph_tag,
    public virtual adjacency_graph_tag,
    public virtual edge_list_graph_tag { };
  
  //=========================================================================
  // Adjacency Matrix Class
  template <typename Directed = directedS,
            typename VertexProperty = no_property,
            typename EdgeProperty = no_property,
            typename GraphProperty = no_property,
            typename Allocator = std::allocator<bool> >
  class adjacency_matrix {
    typedef adjacency_matrix self;
    typedef adjacency_matrix_traits<Directed> Traits;
    
  public:
#if !defined(BOOST_MSVC) || BOOST_MSVC > 1300
    // The bidirectionalS tag is not allowed with the adjacency_matrix
    // graph type. Instead, use directedS, which also provides the
    // functionality required for a Bidirectional Graph (in_edges,
    // in_degree, etc.).
    BOOST_STATIC_ASSERT(!(is_same<Directed, bidirectionalS>::value));
#endif

#ifndef BOOST_GRAPH_NO_BUNDLED_PROPERTIES
    typedef typename detail::retag_property_list<vertex_bundle_t, VertexProperty>::type
      vertex_property_type;
    typedef typename detail::retag_property_list<edge_bundle_t, EdgeProperty>::type
      edge_property_type;
      
  private:
    typedef typename detail::retag_property_list<vertex_bundle_t, VertexProperty>::retagged
      maybe_vertex_bundled;

    typedef typename detail::retag_property_list<edge_bundle_t, EdgeProperty>::retagged
      maybe_edge_bundled;
    
  public:
    // The types that are actually bundled
    typedef typename ct_if<(is_same<maybe_vertex_bundled, no_property>::value),
                           no_vertex_bundle,
                           maybe_vertex_bundled>::type vertex_bundled;
    typedef typename ct_if<(is_same<maybe_edge_bundled, no_property>::value),
                           no_edge_bundle,
                           maybe_edge_bundled>::type edge_bundled;
#else
    typedef EdgeProperty     edge_property_type;
    typedef VertexProperty   vertex_property_type;
    typedef no_vertex_bundle vertex_bundled;
    typedef no_edge_bundle   edge_bundled;
#endif

  public: // should be private
    typedef typename ct_if_t<typename has_property<edge_property_type>::type,
      std::pair<bool, edge_property_type>, char>::type StoredEdge;
#if (defined(BOOST_MSVC) && BOOST_MSVC <= 1300) || defined(BOOST_NO_STD_ALLOCATOR)
    typedef std::vector<StoredEdge> Matrix;
#else
    // This causes internal compiler error for MSVC
    typedef typename Allocator::template rebind<StoredEdge>::other Alloc;
    typedef std::vector<StoredEdge, Alloc> Matrix;
#endif
    typedef typename Matrix::iterator MatrixIter;
    typedef typename Matrix::size_type size_type;
  public:
    // Graph concept required types
    typedef typename Traits::vertex_descriptor vertex_descriptor;
    typedef typename Traits::edge_descriptor edge_descriptor;
    typedef typename Traits::directed_category directed_category;
    typedef typename Traits::edge_parallel_category edge_parallel_category;
    typedef adj_matrix_traversal_tag traversal_category;

    static vertex_descriptor null_vertex()
    {
      return (std::numeric_limits<vertex_descriptor>::max)();
    }
      
    //private: if friends worked, these would be private

    typedef detail::dir_adj_matrix_out_edge_iter<
        vertex_descriptor, MatrixIter, size_type, edge_descriptor
    > DirOutEdgeIter;

    typedef detail::undir_adj_matrix_out_edge_iter<
        vertex_descriptor, MatrixIter, size_type, edge_descriptor
    > UnDirOutEdgeIter;

    typedef typename ct_if_t<
        typename Directed::is_directed_t, DirOutEdgeIter, UnDirOutEdgeIter
    >::type unfiltered_out_edge_iter;

    typedef detail::dir_adj_matrix_in_edge_iter<
        vertex_descriptor, MatrixIter, size_type, edge_descriptor
    > DirInEdgeIter;

    typedef detail::undir_adj_matrix_in_edge_iter<
        vertex_descriptor, MatrixIter, size_type, edge_descriptor
    > UnDirInEdgeIter;

    typedef typename ct_if_t<
        typename Directed::is_directed_t, DirInEdgeIter, UnDirInEdgeIter
    >::type unfiltered_in_edge_iter;
    
    typedef detail::adj_matrix_edge_iter<
        Directed, MatrixIter, size_type, edge_descriptor
    > unfiltered_edge_iter;
    
  public:

    // IncidenceGraph concept required types
    typedef filter_iterator<detail::does_edge_exist, unfiltered_out_edge_iter>
      out_edge_iterator;

    typedef size_type degree_size_type;

    // BidirectionalGraph required types
    typedef filter_iterator<detail::does_edge_exist, unfiltered_in_edge_iter>
      in_edge_iterator;

    // AdjacencyGraph required types
     typedef typename adjacency_iterator_generator<self,
       vertex_descriptor, out_edge_iterator>::type adjacency_iterator;

    // VertexListGraph required types
    typedef size_type vertices_size_type;
    typedef integer_range<vertex_descriptor> VertexList;
    typedef typename VertexList::iterator vertex_iterator;

    // EdgeListGraph required types
    typedef size_type edges_size_type;
    typedef filter_iterator<
        detail::does_edge_exist, unfiltered_edge_iter
    > edge_iterator;

    // PropertyGraph required types
    typedef adjacency_matrix_class_tag graph_tag;

    // Constructor required by MutableGraph
    adjacency_matrix(vertices_size_type n_vertices) 
      : m_matrix(Directed::is_directed ? 
                 (n_vertices * n_vertices)
                 : (n_vertices * (n_vertices + 1) / 2)),
      m_vertex_set(0, n_vertices),
      m_vertex_properties(n_vertices),
      m_num_edges(0) { }

#ifndef BOOST_GRAPH_NO_BUNDLED_PROPERTIES
    // Directly access a vertex or edge bundle
    vertex_bundled& operator[](vertex_descriptor v)
    { return get(vertex_bundle, *this)[v]; }

    const vertex_bundled& operator[](vertex_descriptor v) const
    { return get(vertex_bundle, *this)[v]; }

    edge_bundled& operator[](edge_descriptor e)
    { return get(edge_bundle, *this)[e]; }

    const edge_bundled& operator[](edge_descriptor e) const
    { return get(edge_bundle, *this)[e]; }
#endif
    
    //private: if friends worked, these would be private

    typename Matrix::const_reference
    get_edge(vertex_descriptor u, vertex_descriptor v) const {
      if (Directed::is_directed)
        return m_matrix[u * m_vertex_set.size() + v];
      else {
        if (v > u)
          std::swap(u, v);
        return m_matrix[u * (u + 1)/2 + v];
      }
    }
    typename Matrix::reference
    get_edge(vertex_descriptor u, vertex_descriptor v) {
      if (Directed::is_directed)
        return m_matrix[u * m_vertex_set.size() + v];
      else {
        if (v > u)
          std::swap(u, v);
        return m_matrix[u * (u + 1)/2 + v];
      }
    }

    Matrix m_matrix;
    VertexList m_vertex_set;
    std::vector<vertex_property_type> m_vertex_properties;
    size_type m_num_edges;
  };
  
  //=========================================================================
  // Functions required by the AdjacencyMatrix concept 

  template <typename D, typename VP, typename EP, typename GP, typename A>
  std::pair<typename adjacency_matrix<D,VP,EP,GP,A>::edge_descriptor,
            bool>
  edge(typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor u,
       typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor v,
       const adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    bool exists = detail::get_edge_exists(g.get_edge(u,v), 0);
    typename adjacency_matrix<D,VP,EP,GP,A>::edge_descriptor
      e(exists, u, v, &detail::get_property(g.get_edge(u,v)));
    return std::make_pair(e, exists);
  }

  //=========================================================================
  // Functions required by the IncidenceGraph concept 

  // O(1)
  template <typename VP, typename EP, typename GP, typename A>
  std::pair<typename adjacency_matrix<directedS,VP,EP,GP,A>::out_edge_iterator,
            typename adjacency_matrix<directedS,VP,EP,GP,A>::out_edge_iterator>
  out_edges
    (typename adjacency_matrix<directedS,VP,EP,GP,A>::vertex_descriptor u,
     const adjacency_matrix<directedS,VP,EP,GP,A>& g_)
  {
    typedef adjacency_matrix<directedS,VP,EP,GP,A> Graph;
    Graph& g = const_cast<Graph&>(g_);
    typename Graph::vertices_size_type offset = u * g.m_vertex_set.size();
    typename Graph::MatrixIter f = g.m_matrix.begin() + offset;
    typename Graph::MatrixIter l = f + g.m_vertex_set.size();
    typename Graph::unfiltered_out_edge_iter
          first(f, u, g.m_vertex_set.size())
        , last(l, u, g.m_vertex_set.size());
    detail::does_edge_exist pred;
    typedef typename Graph::out_edge_iterator out_edge_iterator;
    return std::make_pair(out_edge_iterator(pred, first, last), 
                          out_edge_iterator(pred, last, last));
  }

  // O(1)
  template <typename VP, typename EP, typename GP, typename A>
  std::pair<
    typename adjacency_matrix<undirectedS,VP,EP,GP,A>::out_edge_iterator,
    typename adjacency_matrix<undirectedS,VP,EP,GP,A>::out_edge_iterator>
  out_edges
    (typename adjacency_matrix<undirectedS,VP,EP,GP,A>::vertex_descriptor u,
     const adjacency_matrix<undirectedS,VP,EP,GP,A>& g_)
  {
    typedef adjacency_matrix<undirectedS,VP,EP,GP,A> Graph;
    Graph& g = const_cast<Graph&>(g_);
    typename Graph::vertices_size_type offset = u * (u + 1) / 2;
    typename Graph::MatrixIter f = g.m_matrix.begin() + offset;
    typename Graph::MatrixIter l = g.m_matrix.end();

    typename Graph::unfiltered_out_edge_iter
        first(f, u, g.m_vertex_set.size())
      , last(l, u, g.m_vertex_set.size());
    
    detail::does_edge_exist pred;
    typedef typename Graph::out_edge_iterator out_edge_iterator;
    return std::make_pair(out_edge_iterator(pred, first, last), 
                          out_edge_iterator(pred, last, last));
  }
  
  // O(N)
  template <typename D, typename VP, typename EP, typename GP, typename A>  
  typename adjacency_matrix<D,VP,EP,GP,A>::degree_size_type
  out_degree(typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor u,
             const adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    typename adjacency_matrix<D,VP,EP,GP,A>::degree_size_type n = 0;
    typename adjacency_matrix<D,VP,EP,GP,A>::out_edge_iterator f, l;
    for (tie(f, l) = out_edges(u, g); f != l; ++f)
      ++n;
    return n;
  }

  // O(1)
  template <typename D, typename VP, typename EP, typename GP, typename A,
    typename Dir, typename Vertex>  
  typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor
  source(const detail::matrix_edge_desc_impl<Dir,Vertex>& e,
         const adjacency_matrix<D,VP,EP,GP,A>&)
  {
    return e.m_source;
  }

  // O(1)
  template <typename D, typename VP, typename EP, typename GP, typename A,
    typename Dir, typename Vertex>  
  typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor
  target(const detail::matrix_edge_desc_impl<Dir,Vertex>& e,
         const adjacency_matrix<D,VP,EP,GP,A>&)
  {
    return e.m_target;
  }

  //=========================================================================
  // Functions required by the BidirectionalGraph concept 

  // O(1)
  template <typename VP, typename EP, typename GP, typename A>
  std::pair<typename adjacency_matrix<directedS,VP,EP,GP,A>::in_edge_iterator,
            typename adjacency_matrix<directedS,VP,EP,GP,A>::in_edge_iterator>
  in_edges
    (typename adjacency_matrix<directedS,VP,EP,GP,A>::vertex_descriptor u,
     const adjacency_matrix<directedS,VP,EP,GP,A>& g_)
  {
    typedef adjacency_matrix<directedS,VP,EP,GP,A> Graph;
    Graph& g = const_cast<Graph&>(g_);
    typename Graph::MatrixIter f = g.m_matrix.begin() + u;
    typename Graph::MatrixIter l = g.m_matrix.end();
    typename Graph::unfiltered_in_edge_iter
        first(f, l, u, g.m_vertex_set.size())
      , last(l, l, u, g.m_vertex_set.size());
    detail::does_edge_exist pred;
    typedef typename Graph::in_edge_iterator in_edge_iterator;
    return std::make_pair(in_edge_iterator(pred, first, last), 
                          in_edge_iterator(pred, last, last));
  }

  // O(1)
  template <typename VP, typename EP, typename GP, typename A>
  std::pair<
    typename adjacency_matrix<undirectedS,VP,EP,GP,A>::in_edge_iterator,
    typename adjacency_matrix<undirectedS,VP,EP,GP,A>::in_edge_iterator>
  in_edges
    (typename adjacency_matrix<undirectedS,VP,EP,GP,A>::vertex_descriptor u,
     const adjacency_matrix<undirectedS,VP,EP,GP,A>& g_)
  {
    typedef adjacency_matrix<undirectedS,VP,EP,GP,A> Graph;
    Graph& g = const_cast<Graph&>(g_);
    typename Graph::vertices_size_type offset = u * (u + 1) / 2;
    typename Graph::MatrixIter f = g.m_matrix.begin() + offset;
    typename Graph::MatrixIter l = g.m_matrix.end();

    typename Graph::unfiltered_in_edge_iter
        first(f, u, g.m_vertex_set.size())
      , last(l, u, g.m_vertex_set.size());
    
    detail::does_edge_exist pred;
    typedef typename Graph::in_edge_iterator in_edge_iterator;
    return std::make_pair(in_edge_iterator(pred, first, last), 
                          in_edge_iterator(pred, last, last));
  }
  
  // O(N)
  template <typename D, typename VP, typename EP, typename GP, typename A>  
  typename adjacency_matrix<D,VP,EP,GP,A>::degree_size_type
  in_degree(typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor u,
             const adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    typename adjacency_matrix<D,VP,EP,GP,A>::degree_size_type n = 0;
    typename adjacency_matrix<D,VP,EP,GP,A>::in_edge_iterator f, l;
    for (tie(f, l) = in_edges(u, g); f != l; ++f)
      ++n;
    return n;
  }

  //=========================================================================
  // Functions required by the AdjacencyGraph concept 

  template <typename D, typename VP, typename EP, typename GP, typename A>
  std::pair<typename adjacency_matrix<D,VP,EP,GP,A>::adjacency_iterator,
            typename adjacency_matrix<D,VP,EP,GP,A>::adjacency_iterator>
  adjacent_vertices
    (typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor u,
     const adjacency_matrix<D,VP,EP,GP,A>& g_)
  {
      typedef adjacency_matrix<D,VP,EP,GP,A> Graph;
      const Graph& cg = static_cast<const Graph&>(g_);
      Graph& g = const_cast<Graph&>(cg);
      typedef typename Graph::adjacency_iterator adjacency_iterator;
      typename Graph::out_edge_iterator first, last;
      boost::tie(first, last) = out_edges(u, g);
      return std::make_pair(adjacency_iterator(first, &g),
                            adjacency_iterator(last, &g));
  }

  //=========================================================================
  // Functions required by the VertexListGraph concept 

  template <typename D, typename VP, typename EP, typename GP, typename A>
  std::pair<typename adjacency_matrix<D,VP,EP,GP,A>::vertex_iterator,
            typename adjacency_matrix<D,VP,EP,GP,A>::vertex_iterator>
  vertices(const adjacency_matrix<D,VP,EP,GP,A>& g_) {
    typedef adjacency_matrix<D,VP,EP,GP,A> Graph;
    Graph& g = const_cast<Graph&>(g_);
    return std::make_pair(g.m_vertex_set.begin(), g.m_vertex_set.end());
  }

  template <typename D, typename VP, typename EP, typename GP, typename A>
  typename adjacency_matrix<D,VP,EP,GP,A>::vertices_size_type
  num_vertices(const adjacency_matrix<D,VP,EP,GP,A>& g) {
    return g.m_vertex_set.size();
  }
  
  //=========================================================================
  // Functions required by the EdgeListGraph concept 
  
  template <typename D, typename VP, typename EP, typename GP, typename A>
  std::pair<typename adjacency_matrix<D,VP,EP,GP,A>::edge_iterator,
            typename adjacency_matrix<D,VP,EP,GP,A>::edge_iterator>
  edges(const adjacency_matrix<D,VP,EP,GP,A>& g_)
  {
    typedef adjacency_matrix<D,VP,EP,GP,A> Graph;
    Graph& g = const_cast<Graph&>(g_);
    
    typename Graph::unfiltered_edge_iter
      first(g.m_matrix.begin(), g.m_matrix.begin(), 
                                    g.m_vertex_set.size()),
      last(g.m_matrix.end(), g.m_matrix.begin(), 
                                    g.m_vertex_set.size());
    detail::does_edge_exist pred;
    typedef typename Graph::edge_iterator edge_iterator;
    return std::make_pair(edge_iterator(pred, first, last),
                          edge_iterator(pred, last, last));
  }

  // O(1)
  template <typename D, typename VP, typename EP, typename GP, typename A>
  typename adjacency_matrix<D,VP,EP,GP,A>::edges_size_type
  num_edges(const adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    return g.m_num_edges;
  }
  
  //=========================================================================
  // Functions required by the MutableGraph concept 

  // O(1)
  template <typename D, typename VP, typename EP, typename GP, typename A>
  std::pair<typename adjacency_matrix<D,VP,EP,GP,A>::edge_descriptor, bool>
  add_edge(typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor u,
           typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor v,
           const EP& ep,
           adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    typedef typename adjacency_matrix<D,VP,EP,GP,A>::edge_descriptor 
      edge_descriptor;
    if (detail::get_edge_exists(g.get_edge(u,v), 0) == false) {
      ++(g.m_num_edges);
      detail::set_property(g.get_edge(u,v), ep, 0);
      detail::set_edge_exists(g.get_edge(u,v), true, 0);
      return std::make_pair
        (edge_descriptor(true, u, v, &detail::get_property(g.get_edge(u,v))), 
         true);
    } else
      return std::make_pair
        (edge_descriptor(true, u, v, &detail::get_property(g.get_edge(u,v))),
         false);
  }
  // O(1)
  template <typename D, typename VP, typename EP, typename GP, typename A>
  std::pair<typename adjacency_matrix<D,VP,EP,GP,A>::edge_descriptor, bool>
  add_edge(typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor u,
           typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor v,
           adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    EP ep;
    return add_edge(u, v, ep, g);
  }

  // O(1)  
  template <typename D, typename VP, typename EP, typename GP, typename A>
  void
  remove_edge(typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor u,
              typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor v,
              adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    --(g.m_num_edges);
    detail::set_edge_exists(g.get_edge(u,v), false, 0);
  }

  // O(1)
  template <typename D, typename VP, typename EP, typename GP, typename A>
  void
  remove_edge(typename adjacency_matrix<D,VP,EP,GP,A>::edge_descriptor e,
              adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    remove_edge(source(e, g), target(e, g), g);
  }

  
  template <typename D, typename VP, typename EP, typename GP, typename A>
  inline typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor
  add_vertex(adjacency_matrix<D,VP,EP,GP,A>& g) {
    // UNDER CONSTRUCTION
    assert(false);
    return *vertices(g).first;
  }

  template <typename D, typename VP, typename EP, typename GP, typename A>
  inline typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor
  add_vertex(const VP& vp, adjacency_matrix<D,VP,EP,GP,A>& g) {
    // UNDER CONSTRUCTION
    assert(false);
    return *vertices(g).first;
  }

  template <typename D, typename VP, typename EP, typename GP, typename A>
  inline void
  remove_vertex(typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor u,
                adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    // UNDER CONSTRUCTION
    assert(false);
  }

  // O(V)
  template <typename VP, typename EP, typename GP, typename A>
  void
  clear_vertex
    (typename adjacency_matrix<directedS,VP,EP,GP,A>::vertex_descriptor u,
     adjacency_matrix<directedS,VP,EP,GP,A>& g)
  {
    typename adjacency_matrix<directedS,VP,EP,GP,A>::vertex_iterator 
      vi, vi_end;
    for (tie(vi, vi_end) = vertices(g); vi != vi_end; ++vi)
      remove_edge(u, *vi, g);
    for (tie(vi, vi_end) = vertices(g); vi != vi_end; ++vi)
      remove_edge(*vi, u, g);
  }

  // O(V)
  template <typename VP, typename EP, typename GP, typename A>
  void
  clear_vertex
    (typename adjacency_matrix<undirectedS,VP,EP,GP,A>::vertex_descriptor u,
     adjacency_matrix<undirectedS,VP,EP,GP,A>& g)
  {
    typename adjacency_matrix<undirectedS,VP,EP,GP,A>::vertex_iterator
      vi, vi_end;
    for (tie(vi, vi_end) = vertices(g); vi != vi_end; ++vi)
      remove_edge(u, *vi, g);
  }

  //=========================================================================
  // Vertex Property Map
  
  template <typename GraphPtr, typename Vertex, typename T, typename R, 
    typename Tag> 
  class adj_matrix_vertex_property_map 
    : public put_get_helper<R,
         adj_matrix_vertex_property_map<GraphPtr, Vertex, T, R, Tag> >
  {
  public:
    typedef T value_type;
    typedef R reference;
    typedef Vertex key_type;
    typedef boost::lvalue_property_map_tag category;
    adj_matrix_vertex_property_map() { }
    adj_matrix_vertex_property_map(GraphPtr g) : m_g(g) { }
    inline reference operator[](key_type v) const {
      return get_property_value(m_g->m_vertex_properties[v], Tag());
    }
    GraphPtr m_g;
  };

  template <class Property, class Vertex>
  struct adj_matrix_vertex_id_map
    : public boost::put_get_helper<Vertex,
        adj_matrix_vertex_id_map<Property, Vertex> >
  {
    typedef Vertex value_type;
    typedef Vertex reference;
    typedef Vertex key_type;
    typedef boost::readable_property_map_tag category;
     adj_matrix_vertex_id_map() { }
    template <class Graph>
    inline adj_matrix_vertex_id_map(const Graph&) { }
    inline value_type operator[](key_type v) const { return v; }
  };

  namespace detail {

    struct adj_matrix_any_vertex_pa {
      template <class Tag, class Graph, class Property>
      struct bind_ {
        typedef typename property_value<Property,Tag>::type Value;
        typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;
        
        typedef adj_matrix_vertex_property_map<Graph*, Vertex, Value, Value&,
          Tag> type;
        typedef adj_matrix_vertex_property_map<const Graph*, Vertex, Value, 
          const Value&, Tag> const_type;
      };
    };
    struct adj_matrix_id_vertex_pa {
      template <class Tag, class Graph, class Property>
      struct bind_ {
        typedef typename Graph::vertex_descriptor Vertex;
        typedef adj_matrix_vertex_id_map<Property, Vertex> type;
        typedef adj_matrix_vertex_id_map<Property, Vertex> const_type;
      };
    };

    template <class Tag>
    struct adj_matrix_choose_vertex_pa_helper {
      typedef adj_matrix_any_vertex_pa type;
    };
    template <>
    struct adj_matrix_choose_vertex_pa_helper<vertex_index_t> {
      typedef adj_matrix_id_vertex_pa type;
    };

    template <class Tag, class Graph, class Property>
    struct adj_matrix_choose_vertex_pa {
      typedef typename adj_matrix_choose_vertex_pa_helper<Tag>::type Helper;
      typedef typename Helper::template bind_<Tag,Graph,Property> Bind;
      typedef typename Bind::type type;
      typedef typename Bind::const_type const_type;
    };

    struct adj_matrix_vertex_property_selector {
      template <class Graph, class Property, class Tag>
      struct bind_ {
        typedef adj_matrix_choose_vertex_pa<Tag,Graph,Property> Choice;
        typedef typename Choice::type type;
        typedef typename Choice::const_type const_type;
      };
    };

  } // namespace detail

  template <>
  struct vertex_property_selector<adjacency_matrix_class_tag> {
    typedef detail::adj_matrix_vertex_property_selector type;
  };
  
  //=========================================================================
  // Edge Property Map


  template <typename Directed, typename Property, typename Vertex, 
    typename T, typename R, typename Tag> 
  class adj_matrix_edge_property_map 
    : public put_get_helper<R,
         adj_matrix_edge_property_map<Directed, Property, Vertex, T, R, Tag> >
  {
  public:
    typedef T value_type;
    typedef R reference;
    typedef detail::matrix_edge_desc_impl<Directed, Vertex> key_type;
    typedef boost::lvalue_property_map_tag category;
    inline reference operator[](key_type e) const {
      Property& p = *(Property*)e.get_property();
      return get_property_value(p, Tag());
    }
  };
  struct adj_matrix_edge_property_selector {
    template <class Graph, class Property, class Tag>
    struct bind_ {
      typedef typename property_value<Property,Tag>::type T;
      typedef typename Graph::vertex_descriptor Vertex;
      typedef adj_matrix_edge_property_map<typename Graph::directed_category,
        Property, Vertex, T, T&, Tag> type;
      typedef adj_matrix_edge_property_map<typename Graph::directed_category,
        Property, Vertex, T, const T&, Tag> const_type;
    };
  };
  template <>
  struct edge_property_selector<adjacency_matrix_class_tag> {
    typedef adj_matrix_edge_property_selector type;
  };

  //=========================================================================
  // Functions required by PropertyGraph

  namespace detail {

    template <typename Property, typename D, typename VP, typename EP, 
              typename GP, typename A>
    typename boost::property_map<adjacency_matrix<D,VP,EP,GP,A>, 
      Property>::type
    get_dispatch(adjacency_matrix<D,VP,EP,GP,A>& g, Property,
                 vertex_property_tag)
    {
      typedef adjacency_matrix<D,VP,EP,GP,A> Graph;
      typedef typename boost::property_map<adjacency_matrix<D,VP,EP,GP,A>, 
        Property>::type PA;
      return PA(&g);
    }
    template <typename Property, typename D, typename VP, typename EP, 
              typename GP, typename A>
    typename boost::property_map<adjacency_matrix<D,VP,EP,GP,A>, 
      Property>::type
    get_dispatch(adjacency_matrix<D,VP,EP,GP,A>&, Property,
                 edge_property_tag)
    {
      typedef typename boost::property_map<adjacency_matrix<D,VP,EP,GP,A>, 
        Property>::type PA;
      return PA();
    }
    template <typename Property, typename D, typename VP, typename EP, 
              typename GP, typename A>
    typename boost::property_map<adjacency_matrix<D,VP,EP,GP,A>, 
      Property>::const_type
    get_dispatch(const adjacency_matrix<D,VP,EP,GP,A>& g, Property,
                 vertex_property_tag)
    {
      typedef adjacency_matrix<D,VP,EP,GP,A> Graph;
      typedef typename boost::property_map<adjacency_matrix<D,VP,EP,GP,A>, 
        Property>::const_type PA;
      return PA(&g);
    }
    template <typename Property, typename D, typename VP, typename EP, 
              typename GP, typename A>
    typename boost::property_map<adjacency_matrix<D,VP,EP,GP,A>, 
      Property>::const_type
    get_dispatch(const adjacency_matrix<D,VP,EP,GP,A>&, Property,
                 edge_property_tag)
    {
      typedef typename boost::property_map<adjacency_matrix<D,VP,EP,GP,A>, 
        Property>::const_type PA;
      return PA();
    }

  } // namespace detail

  template <typename Property, typename D, typename VP, typename EP, 
            typename GP, typename A>
  inline
  typename property_map<adjacency_matrix<D,VP,EP,GP,A>, Property>::type
  get(Property p, adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    typedef typename property_kind<Property>::type Kind;
    return detail::get_dispatch(g, p, Kind());
  }

  template <typename Property, typename D, typename VP, typename EP, 
            typename GP, typename A>
  inline
  typename property_map<adjacency_matrix<D,VP,EP,GP,A>, Property>::const_type
  get(Property p, const adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    typedef typename property_kind<Property>::type Kind;
    return detail::get_dispatch(g, p, Kind());
  }

  template <typename Property, typename D, typename VP, typename EP, 
            typename GP, typename A, typename Key>
  inline
  typename property_traits<
    typename property_map<adjacency_matrix<D,VP,EP,GP,A>, Property>::const_type
  >::value_type
  get(Property p, const adjacency_matrix<D,VP,EP,GP,A>& g,
      const Key& key)
  {
    return get(get(p, g), key);
  }

  template <typename Property, typename D, typename VP, typename EP, 
            typename GP, typename A, typename Key, typename Value>
  inline void
  put(Property p, adjacency_matrix<D,VP,EP,GP,A>& g,
      const Key& key, const Value& value)
  {
    typedef adjacency_matrix<D,VP,EP,GP,A> Graph;
    typedef typename boost::property_map<Graph, Property>::type Map;
    Map pmap = get(p, g);
    put(pmap, key, value);
  }
  
  //=========================================================================
  // Other Functions

  template <typename D, typename VP, typename EP, typename GP, typename A>  
  typename adjacency_matrix<D,VP,EP,GP,A>::vertex_descriptor
  vertex(typename adjacency_matrix<D,VP,EP,GP,A>::vertices_size_type n,
         const adjacency_matrix<D,VP,EP,GP,A>& g)
  {
    return n;
  }

  // Support for bundled properties
#ifndef BOOST_GRAPH_NO_BUNDLED_PROPERTIES
  template <typename Directed, typename VertexProperty, typename EdgeProperty, typename GraphProperty,
            typename Allocator, typename T, typename Bundle>
  inline
  typename property_map<adjacency_matrix<Directed, VertexProperty, EdgeProperty, GraphProperty, Allocator>,
                        T Bundle::*>::type
  get(T Bundle::* p, adjacency_matrix<Directed, VertexProperty, EdgeProperty, GraphProperty, Allocator>& g)
  {
    typedef typename property_map<adjacency_matrix<Directed, VertexProperty, EdgeProperty, GraphProperty, Allocator>,
                                  T Bundle::*>::type
      result_type;
    return result_type(&g, p);
  }

  template <typename Directed, typename VertexProperty, typename EdgeProperty, typename GraphProperty,
            typename Allocator, typename T, typename Bundle>
  inline
  typename property_map<adjacency_matrix<Directed, VertexProperty, EdgeProperty, GraphProperty, Allocator>,
                        T Bundle::*>::const_type
  get(T Bundle::* p, adjacency_matrix<Directed, VertexProperty, EdgeProperty, GraphProperty, Allocator> const & g)
  {
    typedef typename property_map<adjacency_matrix<Directed, VertexProperty, EdgeProperty, GraphProperty, Allocator>,
                                  T Bundle::*>::const_type
      result_type;
    return result_type(&g, p);
  }
    
  template <typename Directed, typename VertexProperty, typename EdgeProperty, typename GraphProperty,
            typename Allocator, typename T, typename Bundle, typename Key>
  inline T
  get(T Bundle::* p, adjacency_matrix<Directed, VertexProperty, EdgeProperty, GraphProperty, Allocator> const & g,
      const Key& key)
  {
    return get(get(p, g), key);
  }

  template <typename Directed, typename VertexProperty, typename EdgeProperty, typename GraphProperty,
            typename Allocator, typename T, typename Bundle, typename Key>
  inline void
  put(T Bundle::* p, adjacency_matrix<Directed, VertexProperty, EdgeProperty, GraphProperty, Allocator>& g,
      const Key& key, const T& value)
  {
    put(get(p, g), key, value);
  }

#endif

} // namespace boost

#endif // BOOST_ADJACENCY_MATRIX_HPP
