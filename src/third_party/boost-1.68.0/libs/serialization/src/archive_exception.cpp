/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// archive_exception.cpp:

// (C) Copyright 2009 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#if (defined _MSC_VER) && (_MSC_VER == 1200)
#  pragma warning (disable : 4786) // too long name, harmless warning
#endif

#include <exception>
#include <string>
#include <cstring>

#define BOOST_ARCHIVE_SOURCE
#include <boost/serialization/config.hpp>
#include <boost/archive/archive_exception.hpp>

namespace boost {
namespace archive {

BOOST_ARCHIVE_DECL
unsigned int
archive_exception::append(unsigned int l, const char * a){
    while(l < (sizeof(m_buffer) - 1)){
        char c = *a++;
        if('\0' == c)
            break;
        m_buffer[l++] = c;
    }
    m_buffer[l] = '\0';
    return l;
}

BOOST_ARCHIVE_DECL
archive_exception::archive_exception(
    exception_code c, 
    const char * e1,
    const char * e2
) BOOST_NOEXCEPT :
    code(c)
{
    unsigned int length = 0;
    switch(code){
    case no_exception:
        length = append(length, "uninitialized exception");
        break;
    case unregistered_class:
        length = append(length, "unregistered class");
        if(NULL != e1){
            length = append(length, " - ");
            length = append(length, e1);
        }    
        break;
    case invalid_signature:
        length = append(length, "invalid signature");
        break;
    case unsupported_version:
        length = append(length, "unsupported version");
        break;
    case pointer_conflict:
        length = append(length, "pointer conflict");
        break;
    case incompatible_native_format:
        length = append(length, "incompatible native format");
        if(NULL != e1){
            length = append(length, " - ");
            length = append(length, e1);
        }    
        break;
    case array_size_too_short:
        length = append(length, "array size too short");
        break;
    case input_stream_error:
        length = append(length, "input stream error");
        if(NULL != e1){
            length = append(length, "-");
            length = append(length, e1);
        }
        if(NULL != e2){
            length = append(length, "-");
            length = append(length, e2);
        }
        break;
    case invalid_class_name:
        length = append(length, "class name too long");
        break;
    case unregistered_cast:
        length = append(length, "unregistered void cast ");
        length = append(length, (NULL != e1) ? e1 : "?");
        length = append(length, "<-");
        length = append(length, (NULL != e2) ? e2 : "?");
        break;
    case unsupported_class_version:
        length = append(length, "class version ");
        length = append(length, (NULL != e1) ? e1 : "<unknown class>");
        break;
    case other_exception:
        // if get here - it indicates a derived exception 
        // was sliced by passing by value in catch
        length = append(length, "unknown derived exception");
        break;
    case multiple_code_instantiation:
        length = append(length, "code instantiated in more than one module");
        if(NULL != e1){
            length = append(length, " - ");
            length = append(length, e1);
        }    
        break;
    case output_stream_error:
        length = append(length, "output stream error");
        if(NULL != e1){
            length = append(length, "-");
            length = append(length, e1);
        }
        if(NULL != e2){
            length = append(length, "-");
            length = append(length, e2);
        }
        break;
    default:
        BOOST_ASSERT(false);
        length = append(length, "programming error");
        break;
    }
}

BOOST_ARCHIVE_DECL
archive_exception::archive_exception(archive_exception const & oth) BOOST_NOEXCEPT :
	std::exception(oth),
	code(oth.code)
{
	std::memcpy(m_buffer,oth.m_buffer,sizeof m_buffer);
}

BOOST_ARCHIVE_DECL
archive_exception::~archive_exception() BOOST_NOEXCEPT_OR_NOTHROW {}

BOOST_ARCHIVE_DECL const char *
archive_exception::what() const BOOST_NOEXCEPT_OR_NOTHROW {
    return m_buffer;
}

BOOST_ARCHIVE_DECL
archive_exception::archive_exception() BOOST_NOEXCEPT :
    code(no_exception)
{}

} // archive
} // boost
