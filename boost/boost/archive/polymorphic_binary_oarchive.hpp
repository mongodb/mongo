#ifndef BOOST_ARCHIVE_POLYMORPHIC_BINARY_OARCHIVE_HPP
#define BOOST_ARCHIVE_POLYMORPHIC_BINARY_OARCHIVE_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// polymorphic_binary_oarchive.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <boost/config.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/detail/polymorphic_oarchive_impl.hpp>

namespace boost { 
namespace archive {

typedef detail::polymorphic_oarchive_impl<
    binary_oarchive_impl<
        binary_oarchive, std::ostream::char_type, std::ostream::traits_type
    >
 > polymorphic_binary_oarchive;

} // namespace archive
} // namespace boost

// required by smart_cast for compilers not implementing 
// partial template specialization
BOOST_BROKEN_COMPILER_TYPE_TRAITS_SPECIALIZATION(
    boost::archive::polymorphic_binary_oarchive
)

#endif // BOOST_ARCHIVE_POLYMORPHIC_BINARY_OARCHIVE_HPP

