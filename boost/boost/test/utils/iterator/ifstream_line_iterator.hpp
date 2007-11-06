//  (C) Copyright Gennadiy Rozental 2004-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: ifstream_line_iterator.hpp,v $
//
//  Version     : $Revision: 1.7 $
//
//  Description : 
// ***************************************************************************

#ifndef BOOST_IFSTREAM_LINE_ITERATOR_HPP_071894GER
#define BOOST_IFSTREAM_LINE_ITERATOR_HPP_071894GER

// Boost
#include <boost/test/utils/iterator/istream_line_iterator.hpp>

// STL
#include <fstream>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

namespace ut_detail {

// ************************************************************************** //
// **************                ifstream_holder               ************** //
// ************************************************************************** //

template<typename CharT>
class ifstream_holder {
public:
    // Constructor
    explicit    ifstream_holder( basic_cstring<CharT const> file_name )
    {
        if( file_name.is_empty() )
            return;

        m_stream.open( file_name.begin(), std::ios::in );
    }

    bool is_valid()
    {
        return m_stream.is_open();
    }

protected:
#ifdef BOOST_CLASSIC_IOSTREAMS
    typedef std::ifstream                                       stream_t;
#else
    typedef std::basic_ifstream<CharT,std::char_traits<CharT> > stream_t;
#endif

    // Data members
    stream_t    m_stream;
};

} // namespace ut_detail

// ************************************************************************** //
// **************         basic_ifstream_line_iterator         ************** //
// ************************************************************************** //

#ifdef BOOST_MSVC
# pragma warning(push)
# pragma warning(disable: 4355) // 'this' : used in base member initializer list
#endif

template<typename CharT>
class basic_ifstream_line_iterator : ut_detail::ifstream_holder<CharT>, public basic_istream_line_iterator<CharT>
{
public:
    basic_ifstream_line_iterator( basic_cstring<CharT const> file_name, CharT delimeter )
    : ut_detail::ifstream_holder<CharT>( file_name ), basic_istream_line_iterator<CharT>( this->m_stream, delimeter ) {}

    explicit basic_ifstream_line_iterator( basic_cstring<CharT const> file_name = basic_cstring<CharT const>() )
    : ut_detail::ifstream_holder<CharT>( file_name ), basic_istream_line_iterator<CharT>( this->m_stream ) {}
};

#ifdef BOOST_MSVC
# pragma warning(default: 4355)
#endif

typedef basic_ifstream_line_iterator<char>      ifstream_line_iterator;
typedef basic_ifstream_line_iterator<wchar_t>   wifstream_line_iterator;

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: ifstream_line_iterator.hpp,v $
//  Revision 1.7  2005/06/11 07:21:23  rogeeff
//  reverse prev fix
//
//  Revision 1.6  2005/06/07 05:08:03  rogeeff
//  gcc fix
//
//  Revision 1.5  2005/02/20 08:27:09  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.4  2005/02/01 06:40:08  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.3  2005/01/30 01:44:14  rogeeff
//  warnings suppressed
//
//  Revision 1.2  2005/01/22 19:22:13  rogeeff
//  implementation moved into headers section to eliminate dependency of included/minimal component on src directory
//
//  Revision 1.1  2005/01/22 18:21:40  rogeeff
//  moved sharable staff into utils
//
// ***************************************************************************

#endif // BOOST_IFSTREAM_LINE_ITERATOR_HPP_071894GER

