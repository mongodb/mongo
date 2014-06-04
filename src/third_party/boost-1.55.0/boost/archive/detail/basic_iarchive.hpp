#ifndef BOOST_ARCHIVE_DETAIL_BASIC_IARCHIVE_HPP
#define BOOST_ARCHIVE_DETAIL_BASIC_IARCHIVE_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_iarchive.hpp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// can't use this - much as I'd like to as borland doesn't support it
// #include <boost/scoped_ptr.hpp>

#include <boost/config.hpp>
#include <boost/noncopyable.hpp>

#include <boost/type_traits/broken_compiler_spec.hpp>
#include <boost/serialization/tracking_enum.hpp>
#include <boost/archive/basic_archive.hpp>
#include <boost/archive/detail/decl.hpp>
#include <boost/archive/detail/abi_prefix.hpp> // must be the last header

namespace boost {
namespace serialization {
    class extended_type_info;
} // namespace serialization

namespace archive {
namespace detail {

class basic_iarchive_impl;
class BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY()) basic_iserializer;
class BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY()) basic_pointer_iserializer;
//////////////////////////////////////////////////////////////////////
// class basic_iarchive - read serialized objects from a input stream
class BOOST_ARCHIVE_DECL(BOOST_PP_EMPTY()) basic_iarchive :
    private boost::noncopyable
{
    friend class basic_iarchive_impl;
    // hide implementation of this class to minimize header conclusion
    // in client code. I couldn't used scoped pointer with borland
    // boost::scoped_ptr<basic_iarchive_impl> pimpl;
    basic_iarchive_impl * pimpl;

    virtual void vload(version_type &t) =  0;
    virtual void vload(object_id_type &t) =  0;
    virtual void vload(class_id_type &t) =  0;
    virtual void vload(class_id_optional_type &t) = 0;
    virtual void vload(class_name_type &t) = 0;
    virtual void vload(tracking_type &t) = 0;
protected:
    basic_iarchive(unsigned int flags);
    // account for bogus gcc warning
    #if defined(__GNUC__)
    virtual
    #endif
    ~basic_iarchive();
public:
    // note: NOT part of the public API.
    void next_object_pointer(void *t);
    void register_basic_serializer(
        const basic_iserializer & bis
    );
    void load_object(
        void *t, 
        const basic_iserializer & bis
    );
    const basic_pointer_iserializer * 
    load_pointer(
        void * & t, 
        const basic_pointer_iserializer * bpis_ptr,
        const basic_pointer_iserializer * (*finder)(
            const boost::serialization::extended_type_info & eti
        )

    );
    // real public API starts here
    void 
    set_library_version(library_version_type archive_library_version);
    library_version_type 
    get_library_version() const;
    unsigned int
    get_flags() const;
    void 
    reset_object_address(const void * new_address, const void * old_address);
    void 
    delete_created_pointers();
};

} // namespace detail
} // namespace archive
} // namespace boost

// required by smart_cast for compilers not implementing 
// partial template specialization
BOOST_TT_BROKEN_COMPILER_SPEC(
    boost::archive::detail::basic_iarchive  
) 

#include <boost/archive/detail/abi_suffix.hpp> // pops abi_suffix.hpp pragmas

#endif //BOOST_ARCHIVE_DETAIL_BASIC_IARCHIVE_HPP
