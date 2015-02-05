//  (C) Copyright Gennadiy Rozental 2004-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision$
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
#include <boost/test/utils/basic_cstring/io.hpp>

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

typedef custom_manip<struct cdata_t> cdata;

inline std::ostream&
operator<<( custom_printer<cdata> const& p, const_string value )
{
    return *p << BOOST_TEST_L( "<![CDATA[" ) << value << BOOST_TEST_L( "]]>" );
}

//____________________________________________________________________________//

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_XML_PRINTER_HPP_071894GER
