//  (C) Copyright Gennadiy Rozental 2004-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: istream_line_iterator.hpp,v $
//
//  Version     : $Revision: 1.5 $
//
//  Description : 
// ***************************************************************************

#ifndef BOOST_ISTREAM_LINE_ITERATOR_HPP_071894GER
#define BOOST_ISTREAM_LINE_ITERATOR_HPP_071894GER

// Boost
#include <boost/test/utils/basic_cstring/basic_cstring.hpp>
#include <boost/test/utils/iterator/input_iterator_facade.hpp>

// STL
#include <iosfwd>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************         basic_istream_line_iterator          ************** //
// ************************************************************************** //

// !! Should we support policy based delimitation

template<typename CharT>
class basic_istream_line_iterator
: public input_iterator_facade<basic_istream_line_iterator<CharT>,
                               std::basic_string<CharT>,
                               basic_cstring<CharT const> > {
    typedef input_iterator_facade<basic_istream_line_iterator<CharT>,
                                  std::basic_string<CharT>,
                                  basic_cstring<CharT const> > base;
#ifdef BOOST_CLASSIC_IOSTREAMS
    typedef std::istream              istream_type;
#else
    typedef std::basic_istream<CharT> istream_type;
#endif
public:
    // Constructors
    basic_istream_line_iterator() {}
    basic_istream_line_iterator( istream_type& input, CharT delimeter )
    : m_input_stream( &input ), m_delimeter( delimeter )
    {
        this->init();
    }
    explicit basic_istream_line_iterator( istream_type& input )
    : m_input_stream( &input ) 
#if BOOST_WORKAROUND(__GNUC__, < 3) && !defined(__SGI_STL_PORT) && !defined(_STLPORT_VERSION)
    , m_delimeter( '\n' )
#else
    , m_delimeter( input.widen( '\n' ) )
#endif
    {
        this->init();
    }

private:
    friend class input_iterator_core_access;

    // increment implementation
    bool                     get()
    {
        return std::getline( *m_input_stream, this->m_value, m_delimeter );
    }

    // Data members
    istream_type* m_input_stream;
    CharT         m_delimeter;
};

typedef basic_istream_line_iterator<char>       istream_line_iterator;
typedef basic_istream_line_iterator<wchar_t>    wistream_line_iterator;

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: istream_line_iterator.hpp,v $
//  Revision 1.5  2005/12/14 05:01:13  rogeeff
//  *** empty log message ***
//
//  Revision 1.4  2005/02/20 08:27:09  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.3  2005/02/01 06:40:08  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.2  2005/01/22 19:22:14  rogeeff
//  implementation moved into headers section to eliminate dependency of included/minimal component on src directory
//
//  Revision 1.1  2005/01/22 18:21:40  rogeeff
//  moved sharable staff into utils
//
// ***************************************************************************

#endif // BOOST_ISTREAM_LINE_ITERATOR_HPP_071894GER

