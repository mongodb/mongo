//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: results_reporter.hpp,v $
//
//  Version     : $Revision: 1.2 $
//
//  Description : defines class unit_test_result that is responsible for 
//  gathering test results and presenting this information to end-user
// ***************************************************************************

#ifndef BOOST_TEST_RESULTS_REPORTER_HPP_021205GER
#define BOOST_TEST_RESULTS_REPORTER_HPP_021205GER

// Boost.Test
#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/detail/fwd_decl.hpp>

// STL
#include <iosfwd>   // for std::ostream&

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

namespace results_reporter {

// ************************************************************************** //
// **************              formatter interface             ************** //
// ************************************************************************** //

class BOOST_TEST_DECL format {
public:
    // Destructor
    virtual ~format() {}

    virtual void    results_report_start( std::ostream& ostr ) = 0;
    virtual void    results_report_finish( std::ostream& ostr ) = 0;

    virtual void    test_unit_report_start( test_unit const&, std::ostream& ostr ) = 0;
    virtual void    test_unit_report_finish( test_unit const&, std::ostream& ostr ) = 0;

    virtual void    do_confirmation_report( test_unit const&, std::ostream& ostr ) = 0;
};

// ************************************************************************** //
// **************              report configuration            ************** //
// ************************************************************************** //

BOOST_TEST_DECL void    set_level( report_level );
BOOST_TEST_DECL void    set_stream( std::ostream& );
BOOST_TEST_DECL void    set_format( output_format );
BOOST_TEST_DECL void    set_format( results_reporter::format* );

// ************************************************************************** //
// **************               report initiation              ************** //
// ************************************************************************** //

BOOST_TEST_DECL void    make_report( report_level l = INV_REPORT_LEVEL, test_unit_id = INV_TEST_UNIT_ID );
inline void             confirmation_report( test_unit_id id = INV_TEST_UNIT_ID )   
{ make_report( CONFIRMATION_REPORT, id ); }
inline void             short_report( test_unit_id id = INV_TEST_UNIT_ID )
{ make_report( SHORT_REPORT, id ); }
inline void             detailed_report( test_unit_id id = INV_TEST_UNIT_ID )
{ make_report( DETAILED_REPORT, id ); }

} // namespace results_reporter

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: results_reporter.hpp,v $
//  Revision 1.2  2005/12/14 05:13:18  rogeeff
//  dll support introduced
//
//  Revision 1.1  2005/02/20 08:27:06  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
// ***************************************************************************

#endif // BOOST_TEST_RESULTS_REPORTER_HPP_021205GER

