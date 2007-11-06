#ifndef BOOST_ARCHIVE_ARCHIVE_EXCEPTION_HPP
#define BOOST_ARCHIVE_ARCHIVE_EXCEPTION_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// archive/archive_exception.hpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <exception>
#include <cassert>

namespace boost {
namespace archive {

//////////////////////////////////////////////////////////////////////
// exceptions thrown by archives
//
class archive_exception : 
    public virtual std::exception
{
public:
    typedef enum {
        no_exception,       // initialized without code
        other_exception,    // any excepton not listed below
        unregistered_class, // attempt to serialize a pointer of an
                            // an unregistered class
        invalid_signature,  // first line of archive does not contain
                            // expected string
        unsupported_version,// archive created with library version
                            // subsequent to this one
        pointer_conflict,   // an attempt has been made to directly
                            // serialization::detail an object
                            // after having already serialzed the same
                            // object through a pointer.  Were this permited,
                            // it the archive load would result in the
                            // creation of an extra copy of the obect.
        incompatible_native_format, // attempt to read native binary format
                            // on incompatible platform
        array_size_too_short,// array being loaded doesn't fit in array allocated
        stream_error,       // i/o error on stream
        invalid_class_name, // class name greater than the maximum permitted.
                            // most likely a corrupted archive or an attempt
                            // to insert virus via buffer overrun method.
        unregistered_cast   // base - derived relationship not registered with 
                            // void_cast_register
    } exception_code;
    exception_code code;
    archive_exception(exception_code c) : 
        code(c)
    {}
    virtual const char *what( ) const throw( )
    {
        const char *msg = "programming error";
        switch(code){
        case no_exception:
            msg = "uninitialized exception";
            break;
        case unregistered_class:
            msg = "unregistered class";
            break;
        case invalid_signature:
            msg = "invalid signature";
            break;
        case unsupported_version:
            msg = "unsupported version";
            break;
        case pointer_conflict:
            msg = "pointer conflict";
            break;
        case incompatible_native_format:
            msg = "incompatible native format";
            break;
        case array_size_too_short:
            msg = "array size too short";
            break;
        case stream_error:
            msg = "stream error";
            break;
        case invalid_class_name:
            msg = "class name too long";
            break;
        case unregistered_cast:
            msg = "unregistered void cast";
            break;
        case other_exception:
            // if get here - it indicates a derived exception 
            // was sliced by passing by value in catch
            msg = "unknown derived exception";
            break;
        default:
            assert(false);
            break;
        }
        return msg;
    }
protected:
    archive_exception() : 
         code(no_exception)
    {}
};

}// namespace archive
}// namespace boost

#endif //BOOST_ARCHIVE_ARCHIVE_EXCEPTION_HPP
