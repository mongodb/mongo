// Copyright (C) 2005-2006 The Trustees of Indiana University.

// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Jeremiah Willcock
//           Douglas Gregor
//           Andrew Lumsdaine

// Two bit per color property map

#ifndef BOOST_TWO_BIT_COLOR_MAP_HPP
#define BOOST_TWO_BIT_COLOR_MAP_HPP

#include <boost/property_map.hpp>
#include <boost/shared_array.hpp>

namespace boost {

enum two_bit_color_type { 
  two_bit_white = 0, 
  two_bit_gray  = 1, 
  two_bit_green = 2, 
  two_bit_black = 3 
};

template <>
struct color_traits<two_bit_color_type>
{
  static two_bit_color_type white() { return two_bit_white; }
  static two_bit_color_type gray()  { return two_bit_gray; }
  static two_bit_color_type green() { return two_bit_green; }
  static two_bit_color_type black() { return two_bit_black; }
};


template<typename IndexMap = identity_property_map>
struct two_bit_color_map 
{
  std::size_t n;
  IndexMap index;
  shared_array<unsigned char> data;

  typedef typename property_traits<IndexMap>::key_type key_type;
  typedef two_bit_color_type value_type;
  typedef void reference;
  typedef read_write_property_map_tag category;

  explicit two_bit_color_map(std::size_t n, const IndexMap& index = IndexMap())
    : n(n), index(index), data(new unsigned char[(n + 3) / 4])
  {
  }
};

template<typename IndexMap>
inline two_bit_color_type
get(const two_bit_color_map<IndexMap>& pm, 
    typename two_bit_color_map<IndexMap>::key_type key) 
{
  typename property_traits<IndexMap>::value_type i = get(pm.index, key);
  assert (i < pm.n);
  return two_bit_color_type((pm.data.get()[i / 4] >> ((i % 4) * 2)) & 3);
}

template<typename IndexMap>
inline void
put(const two_bit_color_map<IndexMap>& pm, 
    typename two_bit_color_map<IndexMap>::key_type key,
    two_bit_color_type value)
{
  typename property_traits<IndexMap>::value_type i = get(pm.index, key);
  assert (i < pm.n);
  assert (value >= 0 && value < 4);
  std::size_t byte_num = i / 4;
  std::size_t bit_position = ((i % 4) * 2);
    pm.data.get()[byte_num] = (pm.data.get()[byte_num] & ~(3 << bit_position))
      | (value << bit_position);
}

template<typename IndexMap>
inline two_bit_color_map<IndexMap>
make_two_bit_color_map(std::size_t n, const IndexMap& index_map)
{
  return two_bit_color_map<IndexMap>(n, index_map);
}

} // end namespace boost

#endif // BOOST_TWO_BIT_COLOR_MAP_HPP
