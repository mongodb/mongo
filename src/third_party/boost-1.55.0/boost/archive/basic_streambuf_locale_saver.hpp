#ifndef BOOST_ARCHIVE_BASIC_STREAMBUF_LOCALE_SAVER_HPP
#define BOOST_ARCHIVE_BASIC_STREAMBUF_LOCALE_SAVER_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_streambuf_local_saver.hpp

// (C) Copyright 2005 Robert Ramey - http://www.rrsd.com

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

// note derived from boost/io/ios_state.hpp
// Copyright 2002, 2005 Daryle Walker.  Use, modification, and distribution
// are subject to the Boost Software License, Version 1.0.  (See accompanying
// file LICENSE_1_0.txt or a copy at <http://www.boost.org/LICENSE_1_0.txt>.)

//  See <http://www.boost.org/libs/io/> for the library's home page.

#ifndef BOOST_NO_STD_LOCALE

#include <locale>     // for std::locale
#include <streambuf>  // for std::basic_streambuf

#include <boost/config.hpp>
#include <boost/noncopyable.hpp>

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable : 4511 4512)
#endif

namespace boost{
namespace archive{

template < typename Ch, class Tr >
class basic_streambuf_locale_saver :
    private boost::noncopyable
{
public:
    typedef ::std::basic_streambuf<Ch, Tr> state_type;
    typedef ::std::locale aspect_type;
    explicit basic_streambuf_locale_saver( state_type &s )
        : s_save_( s ), a_save_( s.getloc() )
        {}
    basic_streambuf_locale_saver( state_type &s, aspect_type const &a )
        : s_save_( s ), a_save_( s.pubimbue(a) )
        {}
    ~basic_streambuf_locale_saver()
        { this->restore(); }
    void  restore()
        { s_save_.pubimbue( a_save_ ); }
private:
    state_type &       s_save_;
    aspect_type const  a_save_;
};

} // archive
} // boost

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

#endif // BOOST_NO_STD_LOCALE
#endif // BOOST_ARCHIVE_BASIC_STREAMBUF_LOCALE_SAVER_HPP
