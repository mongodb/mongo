#ifndef BOOST_ARCHIVE_BINARY_IPRIMITIVE_HPP
#define BOOST_ARCHIVE_BINARY_IPRIMITIVE_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_binary_iprimitive.hpp
//
// archives stored as native binary - this should be the fastest way
// to archive the state of a group of obects.  It makes no attempt to
// convert to any canonical form.

// IN GENERAL, ARCHIVES CREATED WITH THIS CLASS WILL NOT BE READABLE
// ON PLATFORM APART FROM THE ONE THEY ARE CREATED ON

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <iosfwd>
#include <cassert>
#include <locale>
#include <cstring> // std::memcpy
#include <cstddef> // std::size_t
#include <streambuf> // basic_streambuf
#include <string>

#include <boost/config.hpp>
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{ 
    using ::memcpy; 
    using ::size_t;
} // namespace std
#endif

#include <boost/cstdint.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/throw_exception.hpp>
//#include <boost/limits.hpp>
//#include <boost/io/ios_state.hpp>

#include <boost/archive/basic_streambuf_locale_saver.hpp>
#include <boost/archive/archive_exception.hpp>
#include <boost/archive/detail/auto_link_archive.hpp>
#include <boost/archive/detail/abi_prefix.hpp> // must be the last header

namespace boost { 
namespace archive {

/////////////////////////////////////////////////////////////////////////////
// class binary_iarchive - read serialized objects from a input binary stream
template<class Archive, class Elem, class Tr>
class basic_binary_iprimitive
{
#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
    friend class load_access;
protected:
#else
public:
#endif
    std::basic_streambuf<Elem, Tr> & m_sb;
    // return a pointer to the most derived class
    Archive * This(){
        return static_cast<Archive *>(this);
    }
    boost::scoped_ptr<std::locale> archive_locale;
    basic_streambuf_locale_saver<Elem, Tr> locale_saver;

    // main template for serilization of primitive types
    template<class T>
    void load(T & t){
        load_binary(& t, sizeof(T));
    }

    /////////////////////////////////////////////////////////
    // fundamental types that need special treatment
    
    // trap usage of invalid uninitialized boolean 
    void load(bool & t){
        load_binary(& t, sizeof(t));
        int i = t;
        assert(0 == i || 1 == i);
    }
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load(std::string &s);
    #ifndef BOOST_NO_STD_WSTRING
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load(std::wstring &ws);
    #endif
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load(char * t);
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    load(wchar_t * t);

    BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
    init();
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY()) 
    basic_binary_iprimitive(
        std::basic_streambuf<Elem, Tr> & sb, 
        bool no_codecvt
    );
    BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY()) 
    ~basic_binary_iprimitive();
public:
    void
    load_binary(void *address, std::size_t count);
};

template<class Archive, class Elem, class Tr>
inline void
basic_binary_iprimitive<Archive, Elem, Tr>::load_binary(
    void *address, 
    std::size_t count
){
#if 0
    assert(
        static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)()) >= count
    );
    //if(is.fail())
    //    boost::throw_exception(archive_exception(archive_exception::stream_error));
    // note: an optimizer should eliminate the following for char files
    std::size_t s = count / sizeof(BOOST_DEDUCED_TYPENAME IStream::char_type);
    is.read(
        static_cast<BOOST_DEDUCED_TYPENAME IStream::char_type *>(address), 
        s
    );
    // note: an optimizer should eliminate the following for char files
    s = count % sizeof(BOOST_DEDUCED_TYPENAME IStream::char_type);
    if(0 < s){
        if(is.fail())
            boost::throw_exception(archive_exception(archive_exception::stream_error));
        BOOST_DEDUCED_TYPENAME IStream::char_type t;
        is.read(& t, 1);
        std::memcpy(address, &t, s);
    }
#endif
    // note: an optimizer should eliminate the following for char files
    std::streamsize s = count / sizeof(Elem);
    std::streamsize scount = m_sb.sgetn(
        static_cast<Elem *>(address), 
        s
    );
    if(count != static_cast<std::size_t>(s))
        boost::throw_exception(
            archive_exception(archive_exception::stream_error)
        );
    // note: an optimizer should eliminate the following for char files
    s = count % sizeof(Elem);
    if(0 < s){
//        if(is.fail())
//            boost::throw_exception(archive_exception(archive_exception::stream_error));
        Elem t;
        scount = m_sb.sgetn(& t, 1);
        if(count != 1)
            boost::throw_exception(
                archive_exception(archive_exception::stream_error)
            );
        std::memcpy(address, &t, s);
    }
}

} // namespace archive
} // namespace boost

#include <boost/archive/detail/abi_suffix.hpp> // pop pragams

#endif // BOOST_ARCHIVE_BINARY_IPRIMITIVE_HPP
