#ifndef BOOST_ARCHIVE_POLYMORPHIC_OARCHIVE_HPP
#define BOOST_ARCHIVE_POLYMORPHIC_OARCHIVE_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// polymorphic_oarchive.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com .
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cstddef> // size_t
#include <string>

#include <boost/config.hpp>
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{
    using ::size_t;
} // namespace std
#endif

#include <boost/cstdint.hpp>
#include <boost/pfto.hpp>
#include <boost/archive/detail/oserializer.hpp>
#include <boost/archive/detail/interface_oarchive.hpp>
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
    class basic_oarchive;
    class basic_oserializer;
}

class polymorphic_oarchive :
    public detail::interface_oarchive<polymorphic_oarchive>
{
#ifdef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
public:
#else
    friend class detail::interface_oarchive<polymorphic_oarchive>;
    friend class save_access;
#endif
    // primitive types the only ones permitted by polymorphic archives
    virtual void save(const bool t) = 0;

    virtual void save(const char t) = 0;
    virtual void save(const signed char t) = 0;
    virtual void save(const unsigned char t) = 0;
    #ifndef BOOST_NO_CWCHAR
    #ifndef BOOST_NO_INTRINSIC_WCHAR_T
    virtual void save(const wchar_t t) = 0;
    #endif
    #endif
    virtual void save(const short t) = 0;
    virtual void save(const unsigned short t) = 0;
    virtual void save(const int t) = 0;
    virtual void save(const unsigned int t) = 0;
    virtual void save(const long t) = 0;
    virtual void save(const unsigned long t) = 0;
    #if !defined(BOOST_NO_INTRINSIC_INT64_T)
    virtual void save(const boost::int64_t t) = 0;
    virtual void save(const boost::uint64_t t) = 0;
    #endif
    virtual void save(const float t) = 0;
    virtual void save(const double t) = 0;

    // string types are treated as primitives
    virtual void save(const std::string & t) = 0;
    #ifndef BOOST_NO_STD_WSTRING
    virtual void save(const std::wstring & t) = 0;
    #endif

    virtual void save_null_pointer() = 0;
    // used for xml and other tagged formats
    virtual void save_start(const char * name) = 0;
    virtual void save_end(const char * name) = 0;
    virtual void register_basic_serializer(const detail::basic_oserializer & bos) = 0;
    virtual void lookup_basic_helper(
        const boost::serialization::extended_type_info * const eti,
                boost::shared_ptr<void> & sph
    ) = 0;
    virtual void insert_basic_helper(
        const boost::serialization::extended_type_info * const eti,
                shared_ptr<void> & sph
    ) = 0;

    virtual void end_preamble() = 0;

    // msvc and borland won't automatically pass these to the base class so
    // make it explicit here
    template<class T>
    void save_override(T & t, BOOST_PFTO int)
    {
        archive::save(* this, t);
    }
    // special treatment for name-value pairs.
    template<class T>
    void save_override(
                #ifndef BOOST_NO_FUNCTION_TEMPLATE_ORDERING
                const
                #endif
                ::boost::serialization::nvp<T> & t, int
        ){
        save_start(t.name());
        archive::save(* this, t.const_value());
        save_end(t.name());
    }
public:
    // utility functions implemented by all legal archives
    virtual unsigned int get_flags() const = 0;
    virtual unsigned int get_library_version() const = 0;
    virtual void save_binary(const void * t, std::size_t size) = 0;

    virtual void save_object(
        const void *x,
        const detail::basic_oserializer & bos
    ) = 0;
    virtual void save_pointer(
        const void * t,
        const detail::basic_pointer_oserializer * bpos_ptr
    ) = 0;
};

} // namespace archive
} // namespace boost

// required by smart_cast for compilers not implementing
// partial template specialization
BOOST_BROKEN_COMPILER_TYPE_TRAITS_SPECIALIZATION(boost::archive::polymorphic_oarchive)

#endif // BOOST_ARCHIVE_POLYMORPHIC_OARCHIVE_HPP
