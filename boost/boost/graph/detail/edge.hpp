//
//=======================================================================
// Copyright 1997, 1998, 1999, 2000 University of Notre Dame.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#ifndef BOOST_GRAPH_DETAIL_EDGE_HPP
#define BOOST_GRAPH_DETAIL_EDGE_HPP

#if __GNUC__ < 3
#include <iostream>
#else
#include <iosfwd>
#endif

namespace boost {

  namespace  detail {

    template <typename Directed, typename Vertex>
    struct edge_base
    {
      inline edge_base() {} 
      inline edge_base(Vertex s, Vertex d)
        : m_source(s), m_target(d) { }
      Vertex m_source;
      Vertex m_target;
    };

    template <typename Directed, typename Vertex>
    class edge_desc_impl  : public edge_base<Directed,Vertex> {
      typedef edge_desc_impl                              self;
      typedef edge_base<Directed,Vertex> Base;
    public: 
      typedef void                              property_type;
      
      inline edge_desc_impl() : m_eproperty(0) {} 
      
      inline edge_desc_impl(Vertex s, Vertex d, const property_type* eplug)
        : Base(s,d), m_eproperty(const_cast<property_type*>(eplug)) { }
      
      property_type* get_property() { return m_eproperty; }
      const property_type* get_property() const { return m_eproperty; }
      
      //  protected:
      property_type* m_eproperty;
    };
    
    template <class D, class V>
    inline bool
    operator==(const detail::edge_desc_impl<D,V>& a, 
               const detail::edge_desc_impl<D,V>& b)
    {
      return a.get_property() == b.get_property();
    }
    template <class D, class V>
    inline bool
    operator!=(const detail::edge_desc_impl<D,V>& a, 
               const detail::edge_desc_impl<D,V>& b)
    {
      return ! (a.get_property() == b.get_property());
    }

  } //namespace detail
  
} // namespace boost

namespace std {

#if __GNUC__ < 3
  template <class D, class V>
  std::ostream& 
  operator<<(std::ostream& os, const boost::detail::edge_desc_impl<D,V>& e)
  {
    return os << "(" << e.m_source << "," << e.m_target << ")";
  }
#else
  template <class Char, class Traits, class D, class V>
  std::basic_ostream<Char, Traits>& 
  operator<<(std::basic_ostream<Char, Traits>& os,
             const boost::detail::edge_desc_impl<D,V>& e)
  {
    return os << "(" << e.m_source << "," << e.m_target << ")";
  }
#endif

}


#endif // BOOST_GRAPH_DETAIL_DETAIL_EDGE_HPP
