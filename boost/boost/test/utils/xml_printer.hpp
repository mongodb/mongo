//  (C) Copyright Gennadiy Rozental 2004-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: xml_printer.hpp,v $
//
//  Version     : $Revision: 1.7 $
//
//  Description : common code used by any agent serving as XML printer
// ***************************************************************************

#ifndef BOOST_TEST_XML_PRINTER_HPP_071894GER
#define BOOST_TEST_XML_PRINTER_HPP_071894GER

// Boost.Test
#include <boost/test/utils/basic_cstring/basic_cstring.hpp>
#include <boost/test/utils/fixed_mapping.hpp>
#include <boost/test/utils/custom_manip.hpp>
#include <boost/test/utils/foreach.hpp>

// Boost
#include <boost/config.hpp>

// STL
#include <iostream>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************               xml print helpers              ************** //
// ************************************************************************** //

inline void
print_escaped( std::ostream& where_to, const_string value )
{
    static fixed_mapping<char,char const*> char_type(
        '<' , "lt",
        '>' , "gt",
        '&' , "amp",
        '\'', "apos" ,
        '"' , "quot",

        0
    );

    BOOST_TEST_FOREACH( char, c, value ) {
        char const* ref = char_type[c];

        if( ref )
            where_to << '&' << ref << ';';
        else
            where_to << c;
    }
}

//____________________________________________________________________________//

inline void
print_escaped( std::ostream& where_to, std::string const& value )
{
        print_escaped( where_to, const_string( value ) );
}

//____________________________________________________________________________//

template<typename T>
inline void
print_escaped( std::ostream& where_to, T const& value )
{
        where_to << value;
}

//____________________________________________________________________________//

typedef custom_manip<struct attr_value_t> attr_value;

template<typename T>
inline std::ostream&
operator<<( custom_printer<attr_value> const& p, T const& value )
{
        *p << "=\"";
        print_escaped( *p, value );
        *p << '"';

        return *p;
}

//____________________________________________________________________________//

typedef custom_manip<struct pcdata_t> pcdata;

inline std::ostream&
operator<<( custom_printer<pcdata> const& p, const_string value )
{
    print_escaped( *p, value );

    return *p;
}

//____________________________________________________________________________//

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//
//  $Log: xml_printer.hpp,v $
//  Revision 1.7  2005/07/14 15:50:28  dgregor
//  Untabify
//
//  Revision 1.6  2005/04/29 06:31:18  rogeeff
//  bug fix for incorrect XML output
//
//  Revision 1.5  2005/02/20 08:27:08  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.4  2005/02/01 06:40:08  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.3  2005/01/23 09:59:34  vawjr
//  Changed - all the \r\r\n to \r\n in the windows flavor of the file
//            because VC++ 8.0 complains and refuses to compile
//
//  Revision 1.2  2005/01/22 19:22:13  rogeeff
//  implementation moved into headers section to eliminate dependency of included/minimal component on src directory
//
//  Revision 1.1  2005/01/22 18:21:40  rogeeff
//  moved sharable staff into utils
//
//  Revision 1.3  2005/01/21 07:31:44  rogeeff
//  xml helper facilities reworked to present manipulator interfaces
//
// ***************************************************************************

#endif // BOOST_TEST_XML_PRINTER_HPP_071894GER
