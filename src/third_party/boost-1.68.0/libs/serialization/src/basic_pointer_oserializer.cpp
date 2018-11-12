/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_pointer_oserializer.cpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#define BOOST_ARCHIVE_SOURCE
#include <boost/serialization/config.hpp>
#include <boost/archive/detail/basic_pointer_oserializer.hpp>

namespace boost {
namespace archive {
namespace detail {

BOOST_ARCHIVE_DECL 
basic_pointer_oserializer::basic_pointer_oserializer(
    const boost::serialization::extended_type_info & eti
) :
    basic_serializer(eti)
{}

BOOST_ARCHIVE_DECL 
basic_pointer_oserializer::~basic_pointer_oserializer() {}

} // namespace detail
} // namespace archive
} // namespace boost
