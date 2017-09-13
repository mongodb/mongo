/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_archive.cpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

//////////////////////////////////////////////////////////////////////
//
//  objects are stored as
//
//      class_id*   // -1 for a null pointer
//      if a new class id
//      [
//          exported key - class name*
//          tracking level - always/never
//          class version
//      ]
//
//      if tracking
//      [
//          object_id
//      ]
//          
//      [   // if a new object id
//          data...
//      ]
//
//  * required only for pointers - optional for objects

#define BOOST_ARCHIVE_SOURCE
#include <boost/archive/basic_archive.hpp>

namespace boost {
namespace archive {

///////////////////////////////////////////////////////////////////////
// constants used in archive signature
//This should never ever change. note that is not an std::string
// string.
BOOST_SYMBOL_VISIBLE const char * 
BOOST_ARCHIVE_SIGNATURE(){
    return "serialization::archive";
}

// this should change if the capabilities are added to the library
// such that archives can be created which can't be read by previous
// versions of this library
// 1 - initial version
// 2 - made address tracking optional
// 3 - numerous changes - can't guarentee compatibility with previous versions
// 4 - Boost 1.34
//     added item_version to properly support versioning for collections 
// 5 - Boost 1.36
//     changed serialization of collections: adding version even for primitive
//     types caused backwards compatibility breaking change in 1.35
// 6 - Boost 1.41 17 Nov 2009
//     serializing collection sizes as std::size_t
// 7   Boost 1.42 2 Feb 2010
//     error - changed binary version to 16 bits w/o changing library version #
//     That is - binary archives are recorded with #6 even though they are
//     different from the previous versions.  This means that binary archives
//     created with versions 1.42 and 1.43 will have to be fixed with a special
//     program which fixes the library version # in the header
//     Boost 1.43 6 May 2010
//     no change
// 8 - Boost 1.44
//     separated version_type into library_version_type and class_version_type
//     changed version_type to be stored as 8 bits.
// 10- fixed base64 output/input.
// 11- not changes
// 12- improved serialization of collections
// 13- simplified visibility, removed Borland, removed pfto
// 14- improved visibility, refactor map/set

BOOST_SYMBOL_VISIBLE library_version_type
BOOST_ARCHIVE_VERSION(){
    return library_version_type(14);
}

} // namespace archive
} // namespace boost
