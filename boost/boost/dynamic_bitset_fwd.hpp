// --------------------------------------------------
//
// (C) Copyright Chuck Allison and Jeremy Siek 2001 - 2002.
// (C) Copyright Gennaro Prota                 2003 - 2004.
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// -----------------------------------------------------------

//  See http://www.boost.org/libs/dynamic_bitset for documentation.


#ifndef BOOST_DYNAMIC_BITSET_FWD_HPP
#define BOOST_DYNAMIC_BITSET_FWD_HPP

#include <memory>

namespace boost {

template <typename Block = unsigned long,
          typename Allocator = std::allocator<Block> >
class dynamic_bitset;

} // namespace boost

#endif // include guard
