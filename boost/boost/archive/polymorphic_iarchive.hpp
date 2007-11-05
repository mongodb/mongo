#ifndef BOOST_ARCHIVE_POLYMORPHIC_IARCHIVE_HPP
#define BOOST_ARCHIVE_POLYMORPHIC_IARCHIVE_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// polymorphic_iarchive.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com .
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cstddef> // std::size_t
#include <boost/config.hpp>

#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{
    using ::size_t;
} // namespace std
#endif

#include <string>
#include <boost/cstdint.hpp>

#include <boost/pfto.hpp>
#include <boost/archive/detail/iserializer.hpp>
#include <boost/archive/detail/interface_iarchive.hpp>
#include <boost/serialization/nvp.hpp>

// determine if its necessary to handle (u)int64_t specifically
// i.e. that its not a synonym for (unsigned) long
// if there is no 64 bit int or if its the same as a long
// we shouldn't define separate functions for int64 data types.
#if defined(BOOST_NO_INT64_T) \
    || (ULONG_MAX != 0xffffffff && ULONG_MAX == 18446744073709551615u) // 2**64 - 1
#   define BOOST_NO_INTRINSIC_INT64_T
#endif

namespace boost {
template<class T>
class shared_ptr;
namespace serialization {
    class extended_type_info;
} // namespace serialization
namespace archive {
namespace detail {
    class basic_iarchive;
    class basic_iserializer;
}

class polymorphic_iarchive :
    public detail::interface_iarchive<polymorphic_iarchive>
{
#ifdef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
public:
#else
    friend class detail::interface_iarchive<polymorphic_iarchive>;
    friend class load_access;
#endif
    // primitive types the only ones permitted by polymorphic archives
    virtual void load(bool & t) = 0;

    virtual void load(char & t) = 0;
    virtual void load(signed char & t) = 0;
    virtual void load(unsigned char & t) = 0;
    #ifndef BOOST_NO_CWCHAR
    #ifndef BOOST_NO_INTRINSIC_WCHAR_T
    virtual void load(wchar_t & t) = 0;
    #endif
    #endif
    virtual void load(short & t) = 0;
    virtual void load(unsigned short & t) = 0;
    virtual void load(int & t) = 0;
    virtual void load(unsigned int & t) = 0;
    virtual void load(long & t) = 0;
    virtual void load(unsigned long & t) = 0;

    #if !defined(BOOST_NO_INTRINSIC_INT64_T)
    virtual void load(boost::int64_t & t) = 0;
    virtual void load(boost::uint64_t & t) = 0;
    #endif
    virtual void load(float & t) = 0;
    virtual void load(double & t) = 0;

    // string types are treated as primitives
    virtual void load(std::string & t) = 0;
    #ifndef BOOST_NO_STD_WSTRING
    virtual void load(std::wstring & t) = 0;
    #endif

    // used for xml and other tagged formats
    virtual void load_start(const char * name) = 0;
    virtual void load_end(const char * name) = 0;
    virtual void register_basic_serializer(const detail::basic_iserializer & bis) = 0;
    virtual void lookup_basic_helper(
        const boost::serialization::extended_type_info * const eti,
                boost::shared_ptr<void> & sph
    ) = 0;
    virtual void insert_basic_helper(
        const boost::serialization::extended_type_info * const eti,
                boost::shared_ptr<void> & sph
    ) = 0;

    // msvc and borland won't automatically pass these to the base class so
    // make it explicit here
    template<class T>
    void load_override(T & t, BOOST_PFTO int)
    {
        archive::load(* this, t);
    }
    // special treatment for name-value pairs.
    template<class T>
    void load_override(
                #ifndef BOOST_NO_FUNCTION_TEMPLATE_ORDERING
                const
                #endif
                boost::serialization::nvp<T> & t,
                int
        ){
        load_start(t.name());
        archive::load(* this, t.value());
        load_end(t.name());
    }
public:
    // utility function implemented by all legal archives
    virtual void set_library_version(unsigned int archive_library_version) = 0;
    virtual unsigned int get_library_version() const = 0;
    virtual unsigned int get_flags() const = 0;
    virtual void delete_created_pointers() = 0;
    virtual void reset_object_address(
        const void * new_address,
        const void * old_address
    ) = 0;

    virtual void load_binary(void * t, std::size_t size) = 0;

    // these are used by the serialization library implementation.
    virtual void load_object(
        void *t,
        const detail::basic_iserializer & bis
    ) = 0;
    virtual const detail::basic_pointer_iserializer * load_pointer(
        void * & t,
        const detail::basic_pointer_iserializer * bpis_ptr,
        const detail::basic_pointer_iserializer * (*finder)(
            const boost::serialization::extended_type_info & type
        )
    ) = 0;
};

} // namespace archive
} // namespace boost

// required by smart_cast for compilers not implementing
// partial template specialization
BOOST_BROKEN_COMPILER_TYPE_TRAITS_SPECIALIZATION(boost::archive::polymorphic_iarchive)

#endif // BOOST_ARCHIVE_POLYMORPHIC_IARCHIVE_HPP
