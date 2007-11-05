//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: framework.hpp,v $
//
//  Version     : $Revision: 1.5 $
//
//  Description : defines framework interface
// ***************************************************************************

#ifndef BOOST_TEST_FRAMEWORK_HPP_020805GER
#define BOOST_TEST_FRAMEWORK_HPP_020805GER

// Boost.Test
#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/detail/fwd_decl.hpp>
#include <boost/test/utils/trivial_singleton.hpp>

#include <boost/test/detail/suppress_warnings.hpp>

// STL
#include <stdexcept>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************                   framework                  ************** //
// ************************************************************************** //

namespace framework {

// initialization
BOOST_TEST_DECL void    init( int argc, char* argv[] );

// mutation access methods
BOOST_TEST_DECL void    register_test_unit( test_case* tc );
BOOST_TEST_DECL void    register_test_unit( test_suite* ts );

BOOST_TEST_DECL void    register_observer( test_observer& );
BOOST_TEST_DECL void    deregister_observer( test_observer& );
BOOST_TEST_DECL void    reset_observers();

BOOST_TEST_DECL master_test_suite_t& master_test_suite();

// constant access methods
BOOST_TEST_DECL test_case const&    current_test_case();
#if BOOST_WORKAROUND(__SUNPRO_CC, BOOST_TESTED_AT(0x530) )
template<typename UnitType>
UnitType const&         get( test_unit_id id )
{
    return static_cast<UnitType const&>( get( id, (test_unit_type)UnitType::type ) );
}
test_unit const&        get( test_unit_id, test_unit_type );
#else
test_unit const&        get( test_unit_id, test_unit_type );
template<typename UnitType>
UnitType const&         get( test_unit_id id )
{
    return static_cast<UnitType const&>( get( id, (test_unit_type)UnitType::type ) );
}
#endif

// test initiation
BOOST_TEST_DECL void    run( test_unit_id = INV_TEST_UNIT_ID, bool continue_test = true );
BOOST_TEST_DECL void    run( test_unit const*, bool continue_test = true );

// public test events dispatchers
BOOST_TEST_DECL void    assertion_result( bool passed );
BOOST_TEST_DECL void    exception_caught( execution_exception const& );
BOOST_TEST_DECL void    test_unit_aborted( test_unit const& );

// ************************************************************************** //
// **************                framework errors              ************** //
// ************************************************************************** //

struct internal_error : std::runtime_error {
    internal_error( const_string m ) : std::runtime_error( std::string( m.begin(), m.size() ) ) {}
};

struct setup_error : std::runtime_error {
    setup_error( const_string m ) : std::runtime_error( std::string( m.begin(), m.size() ) ) {}
};

} // namespace framework

} // unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: framework.hpp,v $
//  Revision 1.5  2006/03/19 07:27:52  rogeeff
//  streamline test setup error message
//
//  Revision 1.4  2005/12/14 05:08:44  rogeeff
//  dll support introduced
//
//  Revision 1.3  2005/03/24 04:02:32  rogeeff
//  portability fixes
//
//  Revision 1.2  2005/03/23 21:02:10  rogeeff
//  Sunpro CC 5.3 fixes
//
//  Revision 1.1  2005/02/20 08:27:05  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
// ***************************************************************************

#endif // BOOST_TEST_FRAMEWORK_HPP_020805GER

