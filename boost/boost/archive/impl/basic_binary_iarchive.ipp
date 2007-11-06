/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_binary_iarchive.ipp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.
#include <string>
#include <cassert>
#include <algorithm>
#include <cstring>

#include <boost/config.hpp> // for BOOST_DEDUCED_TYPENAME
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{ 
    using ::memcpy; 
}
#endif

#include <boost/detail/workaround.hpp>

#include <boost/archive/basic_binary_iarchive.hpp>
//#include <boost/serialization/extended_type_info.hpp>

namespace boost {
namespace archive {

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// implementation of binary_binary_archive
template<class Archive>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_iarchive<Archive>::load_override(class_name_type & t, int){
    std::string cn;
    cn.reserve(BOOST_SERIALIZATION_MAX_KEY_SIZE);
    load_override(cn, 0);
    if(cn.size() > (BOOST_SERIALIZATION_MAX_KEY_SIZE - 1))
        boost::throw_exception(
            archive_exception(archive_exception::invalid_class_name)
        );
    std::memcpy(t, cn.data(), cn.size());
    // borland tweak
    t.t[cn.size()] = '\0';
}

template<class Archive>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_iarchive<Archive>::init(){
    // read signature in an archive version independent manner
    std::string file_signature;
    * this->This() >> file_signature;
    if(file_signature != ARCHIVE_SIGNATURE())
        boost::throw_exception(
            archive_exception(archive_exception::invalid_signature)
        );

    // make sure the version of the reading archive library can
    // support the format of the archive being read
    version_type input_library_version;
    * this->This() >> input_library_version;
    
    #if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3205))
    this->set_library_version(input_library_version);
    #else
    #if ! BOOST_WORKAROUND(BOOST_MSVC, <= 1200)
    detail::
    #endif
    basic_iarchive::set_library_version(input_library_version);
    #endif
    
    // extra little .t is to get around borland quirk
    if(ARCHIVE_VERSION() < input_library_version.t)
        boost::throw_exception(
            archive_exception(archive_exception::unsupported_version)
        );
}

} // namespace archive
} // namespace boost
