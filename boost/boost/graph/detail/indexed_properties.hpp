// Copyright 2005 The Trustees of Indiana University.

// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Jeremiah Willcock
//           Douglas Gregor
//           Andrew Lumsdaine

// Indexed properties -- used for CSR and CSR-like graphs

#ifndef BOOST_GRAPH_INDEXED_PROPERTIES_HPP
#define BOOST_GRAPH_INDEXED_PROPERTIES_HPP

#include <vector>
#include <utility>
#include <algorithm>
#include <climits>
#include <iterator>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/properties.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/integer.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/mpl/if.hpp>

namespace boost {
namespace detail {

template<typename Derived, typename Property, typename Descriptor>
class indexed_vertex_properties
{
public:
  typedef no_property vertex_property_type;
  typedef Property vertex_bundled;

  // Directly access a vertex or edge bundle
  Property& operator[](Descriptor v)
  { return m_vertex_properties[get(vertex_index, derived(), v)]; }

  const Property& operator[](Descriptor v) const
  { return m_vertex_properties[get(vertex_index, derived(), v)]; }

protected:
  // Default-construct with no property values
  indexed_vertex_properties() {}

  // Initialize with n default-constructed property values
  indexed_vertex_properties(std::size_t n) : m_vertex_properties(n) { }

  // Resize the properties vector
  void resize(std::size_t n)
  {
    m_vertex_properties.resize(n);
  }

  // Reserve space in the vector of properties
  void reserve(std::size_t n)
  {
    m_vertex_properties.reserve(n);
  }

  // Add a new property value to the back
  void push_back(const Property& prop)
  {
    m_vertex_properties.push_back(prop);
  }

  // Access to the derived object
  Derived& derived() { return *static_cast<Derived*>(this); }

  const Derived& derived() const
  { return *static_cast<const Derived*>(this); }

public: // should be private, but friend templates not portable
  std::vector<Property> m_vertex_properties;
};

template<typename Derived, typename Descriptor>
class indexed_vertex_properties<Derived, void, Descriptor>
{
  struct secret {};

 public:
  typedef no_property vertex_property_type;
  typedef void vertex_bundled;

  secret operator[](secret) { return secret(); }

 protected:
  // All operations do nothing.
  indexed_vertex_properties() { }
  indexed_vertex_properties(std::size_t) { }
  void resize(std::size_t) { }
  void reserve(std::size_t) { }
};

template<typename Derived, typename Property, typename Descriptor>
class indexed_edge_properties
{
public:
  typedef no_property edge_property_type;
  typedef Property edge_bundled;
  typedef Property edge_push_back_type;

  // Directly access a edge or edge bundle
  Property& operator[](Descriptor v)
  { return m_edge_properties[get(edge_index, derived(), v)]; }

  const Property& operator[](Descriptor v) const
  { return m_edge_properties[get(edge_index, derived(), v)]; }

protected:
  // Default-construct with no property values
  indexed_edge_properties() {}

  // Initialize with n default-constructed property values
  indexed_edge_properties(std::size_t n) : m_edge_properties(n) { }

  // Resize the properties vector
  void resize(std::size_t n)
  {
    m_edge_properties.resize(n);
  }

  // Reserve space in the vector of properties
  void reserve(std::size_t n)
  {
    m_edge_properties.reserve(n);
  }

 public:
  // Add a new property value to the back
  void push_back(const Property& prop)
  {
    m_edge_properties.push_back(prop);
  }

 private:
  // Access to the derived object
  Derived& derived() { return *static_cast<Derived*>(this); }

  const Derived& derived() const
  { return *static_cast<const Derived*>(this); }

public: // should be private, but friend templates not portable
  std::vector<Property> m_edge_properties;
};

template<typename Derived, typename Descriptor>
class indexed_edge_properties<Derived, void, Descriptor>
{
  struct secret {};

 public:
  typedef no_property edge_property_type;
  typedef void edge_bundled;
  typedef void* edge_push_back_type;

  secret operator[](secret) { return secret(); }

 protected:
  // All operations do nothing.
  indexed_edge_properties() { }
  indexed_edge_properties(std::size_t) { }
  void resize(std::size_t) { }
  void reserve(std::size_t) { }

 public:
  void push_back(const edge_push_back_type&) { }
};

}
}

#endif // BOOST_GRAPH_INDEXED_PROPERTIES_HPP
