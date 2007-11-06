//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: results_reporter.ipp,v $
//
//  Version     : $Revision: 1.5 $
//
//  Description : result reporting facilties
// ***************************************************************************

#ifndef BOOST_TEST_RESULTS_REPORTER_IPP_020105GER
#define BOOST_TEST_RESULTS_REPORTER_IPP_020105GER

// Boost.Test
#include <boost/test/results_reporter.hpp>
#include <boost/test/unit_test_suite_impl.hpp>
#include <boost/test/results_collector.hpp>
#include <boost/test/framework.hpp>
#include <boost/test/output/plain_report_formatter.hpp>
#include <boost/test/output/xml_report_formatter.hpp>

#include <boost/test/detail/wrap_io_saver.hpp>

// Boost
#include <boost/scoped_ptr.hpp>

// STL
#include <iostream>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

namespace results_reporter {

// ************************************************************************** //
// **************        result reporter implementation        ************** //
// ************************************************************************** //

namespace {

struct results_reporter_impl : test_tree_visitor {
    // Constructor
    results_reporter_impl()
    : m_output( &std::cerr )
    , m_stream_state_saver( new io_saver_type( std::cerr ) )
    , m_report_level( CONFIRMATION_REPORT )
    , m_formatter( new output::plain_report_formatter )
    {}

    // test tree visitor interface implementation
    void    visit( test_case const& tc )
    {
        m_formatter->test_unit_report_start( tc, *m_output );
        m_formatter->test_unit_report_finish( tc, *m_output );
    }
    bool    test_suite_start( test_suite const& ts )
    {
        m_formatter->test_unit_report_start( ts, *m_output );

        if( m_report_level == DETAILED_REPORT && !results_collector.results( ts.p_id ).p_skipped )
            return true;

        m_formatter->test_unit_report_finish( ts, *m_output );
        return false;
    }
    void    test_suite_finish( test_suite const& ts )
    {
        m_formatter->test_unit_report_finish( ts, *m_output );
    }

    typedef scoped_ptr<io_saver_type> saver_ptr;

    // Data members
    std::ostream*       m_output;
    saver_ptr           m_stream_state_saver;
    report_level        m_report_level;
    scoped_ptr<format>  m_formatter;
};

results_reporter_impl& s_rr_impl() { static results_reporter_impl the_inst; return the_inst; }

} // local namespace

// ************************************************************************** //
// **************              report configuration            ************** //
// ************************************************************************** //

void
set_level( report_level l )
{
    if( l != INV_REPORT_LEVEL )
        s_rr_impl().m_report_level = l;
}

//____________________________________________________________________________//

void
set_stream( std::ostream& ostr )
{
    s_rr_impl().m_output = &ostr;
    s_rr_impl().m_stream_state_saver.reset( new io_saver_type( ostr ) );
}

//____________________________________________________________________________//

void
set_format( output_format rf )
{
    switch( rf ) {
    case CLF:
        set_format( new output::plain_report_formatter );
        break;
    case XML:
        set_format( new output::xml_report_formatter );
        break;
    }
}

//____________________________________________________________________________//

void
set_format( results_reporter::format* f )
{
    if( f )
        s_rr_impl().m_formatter.reset( f );
}

//____________________________________________________________________________//

// ************************************************************************** //
// **************               report initiation              ************** //
// ************************************************************************** //

void
make_report( report_level l, test_unit_id id )
{
    if( l == INV_REPORT_LEVEL )
        l = s_rr_impl().m_report_level;

    if( l == NO_REPORT )
        return;

    if( id == INV_TEST_UNIT_ID )
        id = framework::master_test_suite().p_id;

    s_rr_impl().m_stream_state_saver->restore();

    report_level bkup = s_rr_impl().m_report_level;
    s_rr_impl().m_report_level = l;

    s_rr_impl().m_formatter->results_report_start( *s_rr_impl().m_output );

    switch( l ) {
    case CONFIRMATION_REPORT:
        s_rr_impl().m_formatter->do_confirmation_report( framework::get<test_unit>( id ), *s_rr_impl().m_output );
        break;
    case SHORT_REPORT:
    case DETAILED_REPORT:
        traverse_test_tree( id, s_rr_impl() );
        break;
    default:
        break;
    }

    s_rr_impl().m_formatter->results_report_finish( *s_rr_impl().m_output );
    s_rr_impl().m_report_level = bkup;
}

//____________________________________________________________________________//

} // namespace results_reporter

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//
//  $Log: results_reporter.ipp,v $
//  Revision 1.5  2005/12/14 05:57:32  rogeeff
//  *** empty log message ***
//
//  Revision 1.4  2005/04/30 16:48:51  rogeeff
//  io saver warkaround for classic io is shared
//
//  Revision 1.3  2005/04/29 06:27:45  rogeeff
//  bug fix for manipulator nandling
//  bug fix for invalid output stream
//  bug fix for set_format function implementation
//
//  Revision 1.2  2005/02/21 10:12:20  rogeeff
//  Support for random order of test cases implemented
//
//  Revision 1.1  2005/02/20 08:27:07  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
// ***************************************************************************

#endif // BOOST_TEST_RESULTS_REPORTER_IPP_020105GER
