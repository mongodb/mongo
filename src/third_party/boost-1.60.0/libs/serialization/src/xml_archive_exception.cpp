/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// xml_archive_exception.cpp:

// (C) Copyright 2009 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#if (defined _MSC_VER) && (_MSC_VER == 1200)
#  pragma warning (disable : 4786) // too long name, harmless warning
#endif


#include <exception>
#include <boost/assert.hpp>
#include <string>

#define BOOST_ARCHIVE_SOURCE
#include <boost/archive/xml_archive_exception.hpp>

namespace boost {
namespace archive {

BOOST_ARCHIVE_DECL
xml_archive_exception::xml_archive_exception(
        exception_code c, 
        const char * e1,
        const char * e2
    ) : 
        archive_exception(other_exception, e1, e2)
    {
        switch(c){
        case xml_archive_parsing_error:
            archive_exception::append(0, "unrecognized XML syntax");
            break;
        case xml_archive_tag_mismatch:
            archive_exception::append(0, "XML start/end tag mismatch");
            if(NULL != e1){
                archive_exception::append(0, " - ");
                archive_exception::append(0, e1);
            }    
            break;
        case xml_archive_tag_name_error:
            archive_exception::append(0, "Invalid XML tag name");
            break;
        default:
            BOOST_ASSERT(false);
            archive_exception::append(0, "programming error");
            break;
        }
    }
    
BOOST_ARCHIVE_DECL
xml_archive_exception::xml_archive_exception(xml_archive_exception const & oth) :
 	archive_exception(oth)
	{
	}
	
BOOST_ARCHIVE_DECL xml_archive_exception::~xml_archive_exception() BOOST_NOEXCEPT_OR_NOTHROW {}

} // archive
} // boost
