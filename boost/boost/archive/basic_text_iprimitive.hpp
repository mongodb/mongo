#ifndef BOOST_ARCHIVE_BASIC_TEXT_IPRIMITIVE_HPP
#define BOOST_ARCHIVE_BASIC_TEXT_IPRIMITIVE_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_text_iprimitive.hpp

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// archives stored as text - note these ar templated on the basic
// stream templates to accommodate wide (and other?) kind of characters
//
// note the fact that on libraries without wide characters, ostream is
// is not a specialization of basic_ostream which in fact is not defined
// in such cases.   So we can't use basic_ostream<IStream::char_type> but rather
// use two template parameters

#include <cassert>
#include <locale>
#include <cstddef> // size_t

#include <boost/config.hpp>
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{ 
    using ::size_t; 
    #if ! defined(BOOST_DINKUMWARE_STDLIB) && ! defined(__SGI_STL_PORT)
        using ::locale;
    #endif
} // namespace std
#endif

#include <boost/detail/workaround.hpp>
#if BOOST_WORKAROUND(BOOST_DINKUMWARE_STDLIB, == 1)
#include <boost/archive/dinkumware.hpp>
#endif

#include <boost/throw_exception.hpp>
#include <boost/limits.hpp>
#include <boost/io/ios_state.hpp>
#include <boost/scoped_ptr.hpp>

#include <boost/archive/archive_exception.hpp>

#include <boost/archive/detail/abi_prefix.hpp> // must be the last header

namespace boost {
namespace archive {

/////////////////////////////////////////////////////////////////////////
// class basic_text_iarchive - load serialized objects from a input text stream
template<class IStream>
class basic_text_iprimitive
{
#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
protected:
#else
public:
#endif
    IStream &is;
    io::ios_flags_saver flags_saver;
    io::ios_precision_saver precision_saver;
    boost::scoped_ptr<std::locale> archive_locale;
    io::basic_ios_locale_saver<
        BOOST_DEDUCED_TYPENAME IStream::char_type, BOOST_DEDUCED_TYPENAME IStream::traits_type
    > locale_saver;
    template<class T>
    void load(T & t)
    {
        if(is.fail())
            boost::throw_exception(archive_exception(archive_exception::stream_error));
        is >> t;
    }
    void load(unsigned char & t)
    {
        if(is.fail())
            boost::throw_exception(archive_exception(archive_exception::stream_error));
        unsigned short int i;
        is >> i;
        t = static_cast<unsigned char>(i);
    }
    void load(signed char & t)
    {
        if(is.fail())
            boost::throw_exception(archive_exception(archive_exception::stream_error));
        signed short int i;
        is >> i;
        t = static_cast<signed char>(i);
    }
    void load(char & t)
    {
        if(is.fail())
            boost::throw_exception(archive_exception(archive_exception::stream_error));
        short int i;
        is >> i;
        t = static_cast<char>(i);
    }
    #ifndef BOOST_NO_INTRINSIC_WCHAR_T
    void load(wchar_t & t)
    {
        if(is.fail())
            boost::throw_exception(archive_exception(archive_exception::stream_error));
        unsigned i;
        is >> i;
        t = static_cast<wchar_t>(i);
    }
    #endif
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY()) 
    basic_text_iprimitive(IStream  &is, bool no_codecvt);
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY()) 
    ~basic_text_iprimitive();
public:
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load_binary(void *address, std::size_t count);
};

} // namespace archive
} // namespace boost

#include <boost/archive/detail/abi_suffix.hpp> // pop pragams

#endif // BOOST_ARCHIVE_BASIC_TEXT_IPRIMITIVE_HPP
