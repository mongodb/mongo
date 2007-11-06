//  (C) Copyright Gennadiy Rozental 2005.
//  Use, modification, and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: validation.ipp,v $
//
//  Version     : $Revision: 1.1 $
//
//  Description : input validation helpers implementation
// ***************************************************************************

#ifndef BOOST_RT_CLA_VALIDATION_IPP_070604GER
#define BOOST_RT_CLA_VALIDATION_IPP_070604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>

#include <boost/test/utils/runtime/cla/argv_traverser.hpp>
#include <boost/test/utils/runtime/cla/validation.hpp>

// Boost
#include <boost/test/utils/basic_cstring/io.hpp>

// STL

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace cla {

// ************************************************************************** //
// **************           runtime::cla::validation           ************** //
// ************************************************************************** //

BOOST_RT_PARAM_INLINE void
report_input_error( argv_traverser const& tr, format_stream& msg )
{
    if( tr.eoi() )
        msg << BOOST_RT_PARAM_LITERAL( " at the end of input" );
    else {
        msg << BOOST_RT_PARAM_LITERAL( " in the following position: " );

        if( tr.input().size() > 5 )
            msg << tr.input().substr( 0, 5 ) << BOOST_RT_PARAM_LITERAL( "..." );
        else
            msg << tr.input();
    }

    throw BOOST_RT_PARAM_NAMESPACE::logic_error( msg.str() );
}

//____________________________________________________________________________//

} // namespace cla

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

// ************************************************************************** //
//   Revision History:
//
//   $Log: validation.ipp,v $
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //

#endif // BOOST_RT_CLA_VALIDATION_IPP_070604GER
