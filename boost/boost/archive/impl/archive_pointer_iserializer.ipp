/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// pointer_iserializer.ipp: 

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cassert>

#include <boost/config.hpp> // msvc 6.0 needs this for warning suppression

#include <boost/archive/detail/basic_serializer_map.hpp>
#include <boost/archive/detail/archive_pointer_iserializer.hpp>

namespace boost { 
namespace archive {
namespace detail {

template<class Archive>
basic_serializer_map * 
iserializer_map(){
    static bool deleted = false;
    static basic_serializer_map map(deleted);
    return deleted ? NULL : & map;
}

template<class Archive>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
archive_pointer_iserializer<Archive>::archive_pointer_iserializer(
    const boost::serialization::extended_type_info & eti
) :
    basic_pointer_iserializer(eti)
{
    basic_serializer_map *mp = iserializer_map<Archive>();
    assert(NULL != mp);
    mp->insert(this);
}

template<class Archive>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(const basic_pointer_iserializer *) 
archive_pointer_iserializer<Archive>::find(
    const boost::serialization::extended_type_info & eti
){
    basic_serializer_map *mp = iserializer_map<Archive>();
    assert(NULL != mp);
    return static_cast<const basic_pointer_iserializer *>(mp->tfind(eti));
}

template<class Archive>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
archive_pointer_iserializer<Archive>::~archive_pointer_iserializer(){
    // note: we need to check that the map still exists as we can't depend
    // on static variables being constructed in a specific sequence
    basic_serializer_map *mp = iserializer_map<Archive>();
    if(NULL == mp)
        return;
    mp->erase(this);
}

} // namespace detail
} // namespace archive
} // namespace boost
