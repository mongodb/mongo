//  (C) Copyright Gennadiy Rozental 2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: framework.ipp,v $
//
//  Version     : $Revision: 1.10 $
//
//  Description : implements framework API - main driver for the test
// ***************************************************************************

#ifndef BOOST_TEST_FRAMEWORK_IPP_021005GER
#define BOOST_TEST_FRAMEWORK_IPP_021005GER

// Boost.Test
#include <boost/test/framework.hpp>
#include <boost/test/execution_monitor.hpp>
#include <boost/test/unit_test_suite_impl.hpp>
#include <boost/test/unit_test_log.hpp>
#include <boost/test/unit_test_monitor.hpp>
#include <boost/test/test_observer.hpp>
#include <boost/test/results_collector.hpp>
#include <boost/test/progress_monitor.hpp>
#include <boost/test/results_reporter.hpp>
#include <boost/test/test_tools.hpp>

#include <boost/test/detail/unit_test_parameters.hpp>
#include <boost/test/detail/global_typedef.hpp>

#include <boost/test/utils/foreach.hpp>

// Boost
#include <boost/timer.hpp>

// STL
#include <map>
#include <set>
#include <cstdlib>
#include <ctime>

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std { using ::time; using ::srand; }
#endif

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

#ifndef BOOST_TEST_DYN_LINK

// prototype for user's unit test init function
#ifdef BOOST_TEST_ALTERNATIVE_INIT_API
extern bool init_unit_test();
#else
extern boost::unit_test::test_suite* init_unit_test_suite( int argc, char* argv[] );
#endif

#endif

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************            test_start calls wrapper          ************** //
// ************************************************************************** //

namespace ut_detail {

struct test_start_caller {
    test_start_caller( test_observer* to, counter_t tc_amount )
    : m_to( to )
    , m_tc_amount( tc_amount )
    {}

    int operator()()
    {
        m_to->test_start( m_tc_amount );
        return 0;
    }

private:
    // Data members
    test_observer*  m_to;
    counter_t       m_tc_amount;
};

}

// ************************************************************************** //
// **************                   framework                  ************** //
// ************************************************************************** //

class framework_impl : public test_tree_visitor {
public:
    framework_impl()
    : m_master_test_suite( 0 )
    , m_curr_test_case( INV_TEST_UNIT_ID )
    , m_next_test_case_id( MIN_TEST_CASE_ID )
    , m_next_test_suite_id( MIN_TEST_SUITE_ID )
    , m_test_in_progress( false )
    {}

    ~framework_impl()
    {
        BOOST_TEST_FOREACH( test_unit_store::value_type const&, tu, m_test_units ) {
            if( test_id_2_unit_type( tu.second->p_id ) == tut_suite )
                delete  (test_suite const*)tu.second;
            else
                delete  (test_case const*)tu.second;
        }
    }

    void            set_tu_id( test_unit& tu, test_unit_id id ) { tu.p_id.value = id; }

    // test_tree_visitor interface implementation
    void            visit( test_case const& tc )
    {
        if( !tc.check_dependencies() ) {
            BOOST_TEST_FOREACH( test_observer*, to, m_observers )
                to->test_unit_skipped( tc );

            return;
        }

        BOOST_TEST_FOREACH( test_observer*, to, m_observers )
            to->test_unit_start( tc );

        boost::timer tc_timer;
        test_unit_id bkup = m_curr_test_case;
        m_curr_test_case = tc.p_id;
        unit_test_monitor_t::error_level run_result = unit_test_monitor.execute_and_translate( tc );

        unsigned long elapsed = static_cast<unsigned long>( tc_timer.elapsed() * 1e6 );

        if( unit_test_monitor.is_critical_error( run_result ) ) {
            BOOST_TEST_FOREACH( test_observer*, to, m_observers )
                to->test_aborted();
        }

        BOOST_TEST_FOREACH( test_observer*, to, m_observers )
            to->test_unit_finish( tc, elapsed );

        m_curr_test_case = bkup;

        if( unit_test_monitor.is_critical_error( run_result ) )
            throw test_being_aborted();
    }

    bool            test_suite_start( test_suite const& ts )
    {
        if( !ts.check_dependencies() ) {
            BOOST_TEST_FOREACH( test_observer*, to, m_observers )
                to->test_unit_skipped( ts );

            return false;
        }

        BOOST_TEST_FOREACH( test_observer*, to, m_observers )
            to->test_unit_start( ts );

        return true;
    }

    void            test_suite_finish( test_suite const& ts )
    {
        BOOST_TEST_FOREACH( test_observer*, to, m_observers )
            to->test_unit_finish( ts, 0 );
    }

    //////////////////////////////////////////////////////////////////
    struct priority_order {
        bool operator()( test_observer* lhs, test_observer* rhs ) const
        {
            return (lhs->priority() < rhs->priority()) || (lhs->priority() == rhs->priority()) && (lhs < rhs);
        }
    };

    typedef std::map<test_unit_id,test_unit const*> test_unit_store;
    typedef std::set<test_observer*,priority_order> observer_store;

    master_test_suite_t* m_master_test_suite;
    test_unit_id    m_curr_test_case;
    test_unit_store m_test_units;

    test_unit_id    m_next_test_case_id;
    test_unit_id    m_next_test_suite_id;

    bool            m_test_in_progress;

    observer_store  m_observers;
};

//____________________________________________________________________________//

namespace {

framework_impl& s_frk_impl() { static framework_impl the_inst; return the_inst; }

} // local namespace

//____________________________________________________________________________//

namespace framework {

void
init( int argc, char* argv[] )
{
    runtime_config::init( &argc, argv );

    // set the log level nad format
    unit_test_log.set_threshold_level( runtime_config::log_level() );
    unit_test_log.set_format( runtime_config::log_format() );

    // set the report level nad format
    results_reporter::set_level( runtime_config::report_level() );
    results_reporter::set_format( runtime_config::report_format() );

    register_observer( results_collector );
    register_observer( unit_test_log );

    if( runtime_config::show_progress() )
        register_observer( progress_monitor );

    if( runtime_config::detect_memory_leaks() > 0 ) {
        detect_memory_leaks( true );
        break_memory_alloc( runtime_config::detect_memory_leaks() );
    }

    // init master unit test suite
    master_test_suite().argc = argc;
    master_test_suite().argv = argv;

#ifndef BOOST_TEST_DYN_LINK

#ifdef BOOST_TEST_ALTERNATIVE_INIT_API
    if( !init_unit_test() )
        throw setup_error( BOOST_TEST_L("test tree initialization error" ) );
#else
    test_suite* s = init_unit_test_suite( argc, argv );
    if( s )
        master_test_suite().add( s );
#endif

#endif

}

//____________________________________________________________________________//

void
register_test_unit( test_case* tc )
{
    if( tc->p_id != INV_TEST_UNIT_ID )
        throw setup_error( BOOST_TEST_L( "test case already registered" ) );

    test_unit_id new_id = s_frk_impl().m_next_test_case_id;

    if( new_id == MAX_TEST_CASE_ID )
        throw setup_error( BOOST_TEST_L( "too many test cases" ) );

    typedef framework_impl::test_unit_store::value_type map_value_type;

    s_frk_impl().m_test_units.insert( map_value_type( new_id, tc ) );
    s_frk_impl().m_next_test_case_id++;

    s_frk_impl().set_tu_id( *tc, new_id );
}

//____________________________________________________________________________//

void
register_test_unit( test_suite* ts )
{
    if( ts->p_id != INV_TEST_UNIT_ID )
        throw setup_error( BOOST_TEST_L( "test suite already registered" ) );

    test_unit_id new_id = s_frk_impl().m_next_test_suite_id;

    if( new_id == MAX_TEST_SUITE_ID )
        throw setup_error( BOOST_TEST_L( "too many test suites" ) );

    typedef framework_impl::test_unit_store::value_type map_value_type;
    s_frk_impl().m_test_units.insert( map_value_type( new_id, ts ) );
    s_frk_impl().m_next_test_suite_id++;

    s_frk_impl().set_tu_id( *ts, new_id );
}

//____________________________________________________________________________//

void
register_observer( test_observer& to )
{
    s_frk_impl().m_observers.insert( &to );
}

//____________________________________________________________________________//

void
deregister_observer( test_observer& to )
{
    s_frk_impl().m_observers.erase( &to );
}

//____________________________________________________________________________//

void
reset_observers()
{
    s_frk_impl().m_observers.clear();
}

//____________________________________________________________________________//

master_test_suite_t&
master_test_suite()
{
    if( !s_frk_impl().m_master_test_suite )
        s_frk_impl().m_master_test_suite = new master_test_suite_t;

    return *s_frk_impl().m_master_test_suite;
}

//____________________________________________________________________________//

test_case const&
current_test_case()
{
    return get<test_case>( s_frk_impl().m_curr_test_case );
}

//____________________________________________________________________________//

test_unit const&
get( test_unit_id id, test_unit_type t )
{
    test_unit const* res = s_frk_impl().m_test_units[id];

    if( (res->p_type & t) == 0 )
        throw internal_error( "Invalid test unit type" );

    return *res;
}

//____________________________________________________________________________//

void
run( test_unit_id id, bool continue_test )
{
    if( id == INV_TEST_UNIT_ID )
        id = master_test_suite().p_id;

    test_case_counter tcc;
    traverse_test_tree( id, tcc );

    if( tcc.m_count == 0 )
        throw setup_error( BOOST_TEST_L( "test tree is empty" ) );

    bool    call_start_finish   = !continue_test || !s_frk_impl().m_test_in_progress;
    bool    was_in_progress     = s_frk_impl().m_test_in_progress;

    s_frk_impl().m_test_in_progress = true;

    if( call_start_finish ) {
        BOOST_TEST_FOREACH( test_observer*, to, s_frk_impl().m_observers ) {
            boost::execution_monitor em;

            try {
                em.execute( ut_detail::test_start_caller( to, tcc.m_count ) );
            }
            catch( execution_exception const& ex )  {
                throw setup_error( ex.what() );
            }
        }
    }

    switch( runtime_config::random_seed() ) {
    case 0:
        break;
    case 1: {
        unsigned int seed = (unsigned int)std::time( 0 );
        BOOST_TEST_MESSAGE( "Test cases order is shuffled using seed: " << seed );
        std::srand( seed );
        break;
    }
    default:
        BOOST_TEST_MESSAGE( "Test cases order is shuffled using seed: " << runtime_config::random_seed() );
        std::srand( runtime_config::random_seed() );
    }

    try {
        traverse_test_tree( id, s_frk_impl() );
    }
    catch( test_being_aborted const& ) {
        // abort already reported
    }

    if( call_start_finish ) {
        BOOST_TEST_FOREACH( test_observer*, to, s_frk_impl().m_observers )
            to->test_finish();
    }

    s_frk_impl().m_test_in_progress = was_in_progress;
}

//____________________________________________________________________________//

void
run( test_unit const* tu, bool continue_test )
{
    run( tu->p_id, continue_test );
}

//____________________________________________________________________________//

void
assertion_result( bool passed )
{
    BOOST_TEST_FOREACH( test_observer*, to, s_frk_impl().m_observers )
        to->assertion_result( passed );
}

//____________________________________________________________________________//

void
exception_caught( execution_exception const& ex )
{
    BOOST_TEST_FOREACH( test_observer*, to, s_frk_impl().m_observers )
        to->exception_caught( ex );
}

//____________________________________________________________________________//

void
test_unit_aborted( test_unit const& tu )
{
    BOOST_TEST_FOREACH( test_observer*, to, s_frk_impl().m_observers )
        to->test_unit_aborted( tu );
}

//____________________________________________________________________________//

} // namespace framework

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//
//  $Log: framework.ipp,v $
//  Revision 1.10  2006/03/19 07:27:52  rogeeff
//  streamline test setup error message
//
//  Revision 1.9  2006/01/30 07:29:49  rogeeff
//  split memory leaks detection API in two to get more functions with better defined roles
//
//  Revision 1.8  2005/12/17 02:34:11  rogeeff
//  *** empty log message ***
//
//  Revision 1.7  2005/12/14 05:35:57  rogeeff
//  DLL support implemented
//  Alternative init API introduced
//
//  Revision 1.6  2005/05/08 08:55:09  rogeeff
//  typos and missing descriptions fixed
//
//  Revision 1.5  2005/04/05 07:23:20  rogeeff
//  restore default
//
//  Revision 1.4  2005/04/05 06:11:37  rogeeff
//  memory leak allocation point detection\nextra help with _WIN32_WINNT
//
//  Revision 1.3  2005/03/23 21:02:19  rogeeff
//  Sunpro CC 5.3 fixes
//
//  Revision 1.2  2005/02/21 10:12:18  rogeeff
//  Support for random order of test cases implemented
//
//  Revision 1.1  2005/02/20 08:27:07  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
// ***************************************************************************

#endif // BOOST_TEST_FRAMEWORK_IPP_021005GER
