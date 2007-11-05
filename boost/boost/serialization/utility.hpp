#ifndef  BOOST_SERIALIZATION_UTILITY_HPP
#define BOOST_SERIALIZATION_UTILITY_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// serialization/utility.hpp:
// serialization for stl utility templates

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <utility>
#include <boost/config.hpp>

#include <boost/type_traits/remove_const.hpp>
#include <boost/serialization/nvp.hpp>

namespace boost { 
namespace serialization {

// pair
template<class Archive, class F, class S>
inline void serialize(
    Archive & ar,
    std::pair<F, S> & p,
    const unsigned int /* file_version */
){
    // note: we remove any const-ness on the first argument.  The reason is that 
    // for stl maps, the type saved is pair<const key, T).  We remove
    // the const-ness in order to be able to load it.
    typedef BOOST_DEDUCED_TYPENAME boost::remove_const<F>::type typef;
    ar & boost::serialization::make_nvp("first", const_cast<typef &>(p.first));
    ar & boost::serialization::make_nvp("second", p.second);
}

} // serialization
} // namespace boost

#endif // BOOST_SERIALIZATION_UTILITY_HPP
