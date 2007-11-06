#ifndef BOOST_SERIALIZATION_HASH_COLLECTIONS_LOAD_IMP_HPP
#define BOOST_SERIALIZATION_HASH_COLLECTIONS_LOAD_IMP_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
# pragma warning (disable : 4786) // too long name, harmless warning
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// hash_collections_load_imp.hpp: serialization for loading stl collections

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// helper function templates for serialization of hashed collections
#include <boost/config.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/collections_load_imp.hpp>

namespace boost{
namespace serialization {
namespace stl {

//////////////////////////////////////////////////////////////////////
// implementation of serialization for STL containers
//
template<class Archive, class Container, class InputFunction>
inline void load_hash_collection(Archive & ar, Container &s)
{
    s.clear();
    // retrieve number of elements
    unsigned int count;
    unsigned int item_version(0);
    unsigned int bucket_count;;
    ar >> BOOST_SERIALIZATION_NVP(count);
    if(3 < ar.get_library_version()){
       ar >> BOOST_SERIALIZATION_NVP(bucket_count);
       ar >> BOOST_SERIALIZATION_NVP(item_version);
    }
    #if ! defined(__MWERKS__)
    s.resize(bucket_count);
    #endif
    InputFunction ifunc;
    while(count-- > 0){
        ifunc(ar, s, item_version);
    }
}

} // namespace stl 
} // namespace serialization
} // namespace boost

#endif //BOOST_SERIALIZATION_HASH_COLLECTIONS_LOAD_IMP_HPP
