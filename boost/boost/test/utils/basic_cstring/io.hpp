//  (C) Copyright Gennadiy Rozental 2004-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: io.hpp,v $
//
//  Version     : $Revision: 1.5 $
//
//  Description : basic_cstring i/o implementation
// ***************************************************************************

#ifndef  BOOST_TEST_BASIC_CSTRING_IO_HPP_071894GER
#define  BOOST_TEST_BASIC_CSTRING_IO_HPP_071894GER

// Boost.Test
#include <boost/test/utils/basic_cstring/basic_cstring.hpp>

// STL
#include <iosfwd>
#include <string>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

#ifdef BOOST_CLASSIC_IOSTREAMS

template<typename CharT>
inline std::ostream&
operator<<( std::ostream& os, basic_cstring<CharT> const& str )
{
    typedef typename ut_detail::bcs_base_char<CharT>::type char_type;
    char_type const* const beg = reinterpret_cast<char_type const* const>( str.begin() );
    char_type const* const end = reinterpret_cast<char_type const* const>( str.end() );
    os << std::basic_string<char_type>( beg, end - beg );

    return os;
}

#else

template<typename CharT1, typename Tr,typename CharT2>
inline std::basic_ostream<CharT1,Tr>&
operator<<( std::basic_ostream<CharT1,Tr>& os, basic_cstring<CharT2> const& str )
{
    CharT1 const* const beg = reinterpret_cast<CharT1 const*>( str.begin() ); // !!
    CharT1 const* const end = reinterpret_cast<CharT1 const*>( str.end() );
    os << std::basic_string<CharT1,Tr>( beg, end - beg );

    return os;
}

#endif

//____________________________________________________________________________//


} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: io.hpp,v $
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
//  Revision 1.2  2005/01/22 19:22:13  rogeeff
//  implementation moved into headers section to eliminate dependency of included/minimal component on src directory
//
//  Revision 1.1  2005/01/22 18:21:40  rogeeff
//  moved sharable staff into utils
//
// ***************************************************************************

#endif // BOOST_TEST_BASIC_CSTRING_IO_HPP_071894GER
