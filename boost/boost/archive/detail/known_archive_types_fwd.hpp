#ifndef BOOST_ARCHIVE_KNOWN_ARCHIVE_TYPES_FWD_HPP
#define BOOST_ARCHIVE_KNOWN_ARCHIVE_TYPES_FWD_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// known_archive_types_fwd.hpp: set traits of classes to be serialized

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// list of archive type shipped with the serialization system

#include <boost/mpl/list.hpp>

namespace boost {
namespace archive {
namespace detail {

// default implementation of known_archive_types list - 0 elements
// used to generate warning when a polymporhic pointer is serialized
// to an unknown archive - export would otherwise fail silently
template<bool>
struct known_archive_types {
    typedef mpl::list<> type;
};

} // namespace detail
} // namespace archive
} // namespace boost

#endif // BOOST_ARCHIVE_KNOWN_ARCHIVE_TYPES_FWD
