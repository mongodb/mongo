//  (C) Copyright Gennadiy Rozental 2005.
//  Use, modification, and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: validation.hpp,v $
//
//  Version     : $Revision: 1.1 $
//
//  Description : input validation helpers definition
// ***************************************************************************

#ifndef BOOST_RT_CLA_VALIDATION_HPP_062604GER
#define BOOST_RT_CLA_VALIDATION_HPP_062604GER

// Boost.Runtime.Parameter
#include <boost/test/utils/runtime/config.hpp>

#include <boost/test/utils/runtime/cla/fwd.hpp>

namespace boost {

namespace BOOST_RT_PARAM_NAMESPACE {

namespace cla {

// ************************************************************************** //
// **************       runtime::cla::report_input_error       ************** //
// ************************************************************************** //

void report_input_error( argv_traverser const& tr, format_stream& msg );

//____________________________________________________________________________//

#define BOOST_RT_CLA_VALIDATE_INPUT( b, tr, msg ) \
    if( b ) ; else ::boost::BOOST_RT_PARAM_NAMESPACE::cla::report_input_error( tr, format_stream().ref() << msg )

//____________________________________________________________________________//

} // namespace cla

} // namespace BOOST_RT_PARAM_NAMESPACE

} // namespace boost

#ifndef BOOST_RT_PARAM_OFFLINE

#  define BOOST_RT_PARAM_INLINE inline
#  include <boost/test/utils/runtime/cla/validation.ipp>

#endif

// ************************************************************************** //
//   Revision History:
//
//   $Log: validation.hpp,v $
//   Revision 1.1  2005/04/12 06:42:43  rogeeff
//   Runtime.Param library initial commit
//
// ************************************************************************** //

#endif // BOOST_RT_CLA_VALIDATION_HPP_062604GER
