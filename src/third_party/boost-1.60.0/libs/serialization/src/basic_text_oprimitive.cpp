/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_text_oprimitive.cpp:

// (C) Copyright 2004 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#if (defined _MSC_VER) && (_MSC_VER == 1200)
#  pragma warning (disable : 4786) // too long name, harmless warning
#endif

#include <ostream>

#define BOOST_ARCHIVE_SOURCE
#include <boost/archive/detail/auto_link_archive.hpp>
#include <boost/archive/impl/basic_text_oprimitive.ipp>

namespace boost {
namespace archive {

// explicitly instantiate for this type of text stream
template class basic_text_oprimitive<std::ostream> ;

} // namespace archive
} // namespace boost
