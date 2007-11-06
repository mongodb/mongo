#ifndef  BOOST_SERIALIZATION_MAP_HPP
#define BOOST_SERIALIZATION_MAP_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// serialization/map.hpp:
// serialization for stl map templates

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <map>

#include <boost/config.hpp>

#include <boost/serialization/utility.hpp>
#include <boost/serialization/collections_save_imp.hpp>
#include <boost/serialization/collections_load_imp.hpp>
#include <boost/serialization/split_free.hpp>

namespace boost { 
namespace serialization {

template<class Archive, class Type, class Key, class Compare, class Allocator >
inline void save(
    Archive & ar,
    const std::map<Key, Type, Compare, Allocator> &t,
    const unsigned int /* file_version */
){
    boost::serialization::stl::save_collection<
        Archive, 
        std::map<Key, Type, Compare, Allocator> 
    >(ar, t);
}

template<class Archive, class Type, class Key, class Compare, class Allocator >
inline void load(
    Archive & ar,
    std::map<Key, Type, Compare, Allocator> &t,
    const unsigned int /* file_version */
){
    boost::serialization::stl::load_collection<
        Archive,
        std::map<Key, Type, Compare, Allocator>,
        boost::serialization::stl::archive_input_unique<
            Archive, std::map<Key, Type, Compare, Allocator> >,
            boost::serialization::stl::no_reserve_imp<std::map<
                Key, Type, Compare, Allocator
            >
        >
    >(ar, t);
}

// split non-intrusive serialization function member into separate
// non intrusive save/load member functions
template<class Archive, class Type, class Key, class Compare, class Allocator >
inline void serialize(
    Archive & ar,
    std::map<Key, Type, Compare, Allocator> &t,
    const unsigned int file_version
){
    boost::serialization::split_free(ar, t, file_version);
}

// multimap
template<class Archive, class Type, class Key, class Compare, class Allocator >
inline void save(
    Archive & ar,
    const std::multimap<Key, Type, Compare, Allocator> &t,
    const unsigned int /* file_version */
){
    boost::serialization::stl::save_collection<
        Archive, 
        std::multimap<Key, Type, Compare, Allocator> 
    >(ar, t);
}

template<class Archive, class Type, class Key, class Compare, class Allocator >
inline void load(
    Archive & ar,
    std::multimap<Key, Type, Compare, Allocator> &t,
    const unsigned int /* file_version */
){
    boost::serialization::stl::load_collection<
        Archive,
        std::multimap<Key, Type, Compare, Allocator>,
        boost::serialization::stl::archive_input_multi<
            Archive, std::multimap<Key, Type, Compare, Allocator> 
        >,
        boost::serialization::stl::no_reserve_imp<
            std::multimap<Key, Type, Compare, Allocator> 
        >
    >(ar, t);
}

// split non-intrusive serialization function member into separate
// non intrusive save/load member functions
template<class Archive, class Type, class Key, class Compare, class Allocator >
inline void serialize(
    Archive & ar,
    std::multimap<Key, Type, Compare, Allocator> &t,
    const unsigned int file_version
){
    boost::serialization::split_free(ar, t, file_version);
}

} // serialization
} // namespace boost

#endif // BOOST_SERIALIZATION_MAP_HPP
