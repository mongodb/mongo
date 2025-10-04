//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTERPROCESS_FLAT_MAP_INDEX_HPP
#define BOOST_INTERPROCESS_FLAT_MAP_INDEX_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif
#
#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>

// interprocess
#include <boost/container/flat_map.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
// intrusive/detail
#include <boost/intrusive/detail/minimal_pair_header.hpp>         //std::pair
#include <boost/intrusive/detail/minimal_less_equal_header.hpp>   //std::less


//!\file
//!Describes index adaptor of boost::map container, to use it
//!as name/shared memory index

//[flat_map_index
namespace boost { namespace interprocess {

#ifndef BOOST_INTERPROCESS_DOXYGEN_INVOKED

//!Helper class to define typedefs from IndexTraits
template <class MapConfig>
struct flat_map_index_aux
{
   typedef typename MapConfig::key_type            key_type;
   typedef typename MapConfig::mapped_type         mapped_type;
   typedef typename MapConfig::
      segment_manager_base                   segment_manager_base;
   typedef std::less<key_type>                     key_less;
   typedef std::pair<key_type, mapped_type>        value_type;
   typedef allocator<value_type
                    ,segment_manager_base>   allocator_type;
   typedef boost::container::flat_map<key_type,  mapped_type,
                                      key_less, allocator_type>      index_t;
};

#endif   //#ifndef BOOST_INTERPROCESS_DOXYGEN_INVOKED

//!Index type based in flat_map. Just derives from flat_map and
//!defines the interface needed by managed memory segments.
template <class MapConfig>
class flat_map_index
   //Derive class from flat_map specialization
   : private flat_map_index_aux<MapConfig>::index_t
{
   #if !defined(BOOST_INTERPROCESS_DOXYGEN_INVOKED)
   typedef flat_map_index_aux<MapConfig>  index_aux;
   typedef typename index_aux::index_t    base_type;
   typedef typename index_aux::
      segment_manager_base                   segment_manager_base;
   typedef typename base_type::key_type      key_type;
   typedef typename base_type::mapped_type   mapped_type;
   #endif   //#ifndef BOOST_INTERPROCESS_DOXYGEN_INVOKED

   public:
   using base_type::begin;
   using base_type::end;
   using base_type::size;
   using base_type::erase;
   using base_type::shrink_to_fit;
   using base_type::reserve;
   typedef typename base_type::iterator         iterator;
   typedef typename base_type::const_iterator   const_iterator;
   typedef typename base_type::value_type       value_type;
   typedef typename MapConfig::compare_key_type compare_key_type;
   typedef iterator                             insert_commit_data;
   typedef iterator                             index_data_t;

   //!Constructor. Takes a pointer to the segment manager. Can throw
   flat_map_index(segment_manager_base *segment_mngr)
      : base_type(typename index_aux::key_less(),
                  typename index_aux::allocator_type(segment_mngr))
   {}

   std::pair<iterator, bool> insert_check
      (const compare_key_type& key, insert_commit_data&)
   {
      std::pair<iterator, bool> r;
      r.first = this->base_type::find(key_type(key.str(), key.len()));
      r.second = r.first == this->base_type::end();
      return r;
   }

   iterator insert_commit
      (const compare_key_type &k, void *context, index_data_t&, insert_commit_data& )
   {
      //Now commit the insertion using previous context data
      return this->base_type::insert(value_type(key_type(k.str(), k.len()), mapped_type(context))).first;
   }

   iterator find(const compare_key_type& k)
   {  return this->base_type::find(key_type(k.str(), k.len()));   }
};

}}   //namespace boost { namespace interprocess
//]
#include <boost/interprocess/detail/config_end.hpp>

#endif   //#ifndef BOOST_INTERPROCESS_FLAT_MAP_INDEX_HPP
