//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_CONTAINER_FWD_HPP
#define BOOST_CONTAINER_CONTAINER_FWD_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

//////////////////////////////////////////////////////////////////////////////
//                        Standard predeclarations
//////////////////////////////////////////////////////////////////////////////

/// @cond

namespace boost{ 
namespace intrusive{ 
   //Create namespace to avoid compilation errors
}}

namespace boost{ namespace container{ namespace container_detail{

namespace bi = boost::intrusive;

}}}

#include <utility> 
#include <memory> 
#include <functional> 
#include <iosfwd>
#include <string>

/// @endcond

//////////////////////////////////////////////////////////////////////////////
//                             Containers
//////////////////////////////////////////////////////////////////////////////

namespace boost {
namespace container {

//vector class
template <class T
         ,class A = std::allocator<T> >
class vector;

//vector class
template <class T
         ,class A = std::allocator<T> >
class stable_vector;

//vector class
template <class T
         ,class A = std::allocator<T> >
class deque;

//list class
template <class T
         ,class A = std::allocator<T> >
class list;

//slist class
template <class T
         ,class A = std::allocator<T> >
class slist;

//set class
template <class T
         ,class Pred  = std::less<T>
         ,class A = std::allocator<T> >
class set;

//multiset class
template <class T
         ,class Pred  = std::less<T>
         ,class A = std::allocator<T> >
class multiset;

//map class
template <class Key
         ,class T
         ,class Pred  = std::less<Key>
         ,class A = std::allocator<std::pair<const Key, T> > >
class map;

//multimap class
template <class Key
         ,class T
         ,class Pred  = std::less<Key>
         ,class A = std::allocator<std::pair<const Key, T> > >
class multimap;

//flat_set class
template <class T
         ,class Pred  = std::less<T>
         ,class A = std::allocator<T> >
class flat_set;

//flat_multiset class
template <class T
         ,class Pred  = std::less<T>
         ,class A = std::allocator<T> >
class flat_multiset;

//flat_map class
template <class Key
         ,class T
         ,class Pred  = std::less<Key>
         ,class A = std::allocator<std::pair<Key, T> > >
class flat_map;

//flat_multimap class
template <class Key
         ,class T
         ,class Pred  = std::less<Key>
         ,class A = std::allocator<std::pair<Key, T> > >
class flat_multimap;

//basic_string class
template <class CharT
         ,class Traits = std::char_traits<CharT>
         ,class A  = std::allocator<CharT> > 
class basic_string;

//! Type used to tag that the input range is
//! guaranteed to be ordered
struct ordered_range_impl_t {};

//! Type used to tag that the input range is
//! guaranteed to be ordered and unique
struct ordered_unique_range_impl_t{};

/// @cond

typedef ordered_range_impl_t * ordered_range_t;
typedef ordered_unique_range_impl_t *ordered_unique_range_t;

/// @endcond

//! Value used to tag that the input range is
//! guaranteed to be ordered
static const ordered_range_t ordered_range = 0;

//! Value used to tag that the input range is
//! guaranteed to be ordered and unique
static const ordered_unique_range_t ordered_unique_range = 0;

/// @cond

namespace detail_really_deep_namespace {

//Otherwise, gcc issues a warning of previously defined
//anonymous_instance and unique_instance
struct dummy
{
   dummy()
   {
      (void)ordered_range;
      (void)ordered_unique_range;
   }
};

}  //detail_really_deep_namespace {

/// @endcond

}}  //namespace boost { namespace container {

#endif //#ifndef BOOST_CONTAINER_CONTAINER_FWD_HPP
