#ifndef BOOST_ARCHIVE_ARCHIVE_POINTER_ISERIALIZER_POINTER_HPP
#define BOOST_ARCHIVE_ARCHIVE_POINTER_ISERIALIZER_POINTER_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// archive_pointer_iserializer.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cassert>
#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#include <boost/archive/detail/basic_serializer.hpp>
#include <boost/archive/detail/basic_pointer_iserializer.hpp>

#include <boost/archive/detail/abi_prefix.hpp> // must be the last header

namespace boost {

namespace serialization {
    class extended_type_info;
} // namespace serialization

namespace archive {
namespace detail {

template<class Archive>
class archive_pointer_iserializer : 
    public basic_pointer_iserializer {
protected:
    explicit BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY()) 
    archive_pointer_iserializer(
        const boost::serialization::extended_type_info & eti
    );
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY()) 
    // account for bogus gcc warning
    #if defined(__GNUC__)
    virtual
    #endif
    ~archive_pointer_iserializer();
public:
    virtual const basic_iserializer & get_basic_serializer() const 
    // mscvc 6.0 requires template functions to be implemented. For this
    // reason we can't make abstract.
    #if BOOST_WORKAROUND(BOOST_MSVC, <= 1300) || defined(__MWERKS__)
    {
        assert(false);
        return *static_cast<const basic_iserializer *>(NULL);
    }
    #else
    = 0;
    #endif
    virtual void load_object_ptr(
        basic_iarchive  & ar, 
        void * & x,
        const unsigned int file_version
    ) const 
    #if BOOST_WORKAROUND(BOOST_MSVC, <= 1300) || defined(__MWERKS__)
    {
        assert(false);
    }
    #else
    = 0;
    #endif
    // return the type_extended load pointer corresponding to a given
    // type_info.  returns NULL if there is no such instance. This
    // would indicate that the no object of the specified type was loaded
    // any where in the code.
    static
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(const basic_pointer_iserializer *)
    find(
        const boost::serialization::extended_type_info & eti
    );
};

} // namespace detail
} // namespace archive
} // namespace boost

#include <boost/archive/detail/abi_suffix.hpp> // pops abi_suffix.hpp pragmas

#endif // BOOST_ARCHIVE_ARCHIVE_POINTER_ISERIALIZER_POINTER_HPP
