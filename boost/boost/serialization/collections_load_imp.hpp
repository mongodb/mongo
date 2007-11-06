#ifndef  BOOST_SERIALIZATION_COLLECTIONS_LOAD_IMP_HPP
#define BOOST_SERIALIZATION_COLLECTIONS_LOAD_IMP_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#if defined(_MSC_VER) && (_MSC_VER <= 1020)
#  pragma warning (disable : 4786) // too long name, harmless warning
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// collections_load_imp.hpp: serialization for loading stl collections

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// helper function templates for serialization of collections

#include <cassert>
#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#include <boost/aligned_storage.hpp>

#include <boost/serialization/access.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/detail/stack_constructor.hpp>

namespace boost{
namespace serialization {
namespace stl {

//////////////////////////////////////////////////////////////////////
// implementation of serialization for STL containers
//

// sequential container input
template<class Archive, class Container>
struct archive_input_seq
{
    inline void operator()(
        Archive &ar, 
        Container &s, 
        const unsigned int v
    ){
        typedef BOOST_DEDUCED_TYPENAME Container::value_type type;
        detail::stack_construct<Archive, type> t(ar, v);
        // borland fails silently w/o full namespace
        ar >> boost::serialization::make_nvp("item", t.reference());
        s.push_back(t.reference());
        ar.reset_object_address(& s.back() , & t.reference());
    }
};

// map and set input
template<class Archive, class Container>
struct archive_input_unique
{
    inline void operator()(
        Archive &ar, 
        Container &s, 
        const unsigned int v
    ){
        typedef BOOST_DEDUCED_TYPENAME Container::value_type type;
        detail::stack_construct<Archive, type>t(ar, v);
        // borland fails silently w/o full namespace
        ar >> boost::serialization::make_nvp("item", t.reference());
        std::pair<BOOST_DEDUCED_TYPENAME Container::const_iterator, bool> result = 
            s.insert(t.reference());
        if(result.second)
            ar.reset_object_address(& (* result.first), & t.reference());
    }
};

// multiset and multimap input
template<class Archive, class Container>
struct archive_input_multi
{
    inline void operator()(
        Archive &ar, 
        Container &s, 
        const unsigned int v
    ){
        typedef BOOST_DEDUCED_TYPENAME Container::value_type type;
        detail::stack_construct<Archive, type> t(ar, v);
        // borland fails silently w/o full namespace
        ar >> boost::serialization::make_nvp("item", t.reference());
        BOOST_DEDUCED_TYPENAME Container::const_iterator result 
            = s.insert(t.reference());
        ar.reset_object_address(& (* result), & t.reference());
    }
};

template<class Container>
class reserve_imp
{
public:
    void operator()(Container &s, unsigned int count) const {
        s.reserve(count);
    }
};

template<class Container>
class no_reserve_imp
{
public:
    void operator()(Container & /* s */, unsigned int /* count */) const{}
};

template<class Archive, class Container, class InputFunction, class R>
inline void load_collection(Archive & ar, Container &s)
{
    s.clear();
    // retrieve number of elements
    unsigned int count;
    unsigned int item_version(0);
    ar >> BOOST_SERIALIZATION_NVP(count);
    if(3 < ar.get_library_version()){
        ar >> BOOST_SERIALIZATION_NVP(item_version);
    }
    R rx;
    rx(s, count);
    InputFunction ifunc;
    while(count-- > 0){
        ifunc(ar, s, item_version);
    }
}

} // namespace stl 
} // namespace serialization
} // namespace boost

#endif //BOOST_SERIALIZATION_COLLECTIONS_LOAD_IMP_HPP
