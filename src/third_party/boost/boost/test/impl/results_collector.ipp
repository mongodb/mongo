//  (C) Copyright Gennadiy Rozental 2005-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision: 57992 $
//
//  Description : implements Unit Test results collecting facility.
// ***************************************************************************

#ifndef BOOST_TEST_RESULTS_COLLECTOR_IPP_021105GER
#define BOOST_TEST_RESULTS_COLLECTOR_IPP_021105GER

// Boost.Test
#include <boost/test/unit_test_suite_impl.hpp>
#include <boost/test/unit_test_log.hpp>
#include <boost/test/results_collector.hpp>
#include <boost/test/framework.hpp>

// Boost
#include <boost/cstdlib.hpp>

// STL
#include <map>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************                 test_results                 ************** //
// ************************************************************************** //

test_results::test_results()
{
    clear();
}

//____________________________________________________________________________//

bool
test_results::passed() const
{
    return  !p_skipped                                  &&
            p_test_cases_failed == 0                    &&
            p_assertions_failed <= p_expected_failures  &&
            !p_aborted;
}

//____________________________________________________________________________//

int
test_results::result_code() const
{
    return passed() ? exit_success
           : ( (p_assertions_failed > p_expected_failures || p_skipped )
                    ? exit_test_failure
                    : exit_exception_failure );
}

//____________________________________________________________________________//

void
test_results::operator+=( test_results const& tr )
{
    p_assertions_passed.value   += tr.p_assertions_passed;
    p_assertions_failed.value   += tr.p_assertions_failed;
    p_test_cases_passed.value   += tr.p_test_cases_passed;
    p_test_cases_failed.value   += tr.p_test_cases_failed;
    p_test_cases_skipped.value  += tr.p_test_cases_skipped;
    p_test_cases_aborted.value  += tr.p_test_cases_aborted;
}

//____________________________________________________________________________//

void
test_results::clear()
{
    p_assertions_passed.value    = 0;
    p_assertions_failed.value    = 0;
    p_expected_failures.value    = 0;
    p_test_cases_passed.value    = 0;
    p_test_cases_failed.value    = 0;
    p_test_cases_skipped.value   = 0;
    p_test_cases_aborted.value   = 0;
    p_aborted.value              = false;
    p_skipped.value              = true;
}

//____________________________________________________________________________//
    
// ************************************************************************** //
// **************               results_collector              ************** //
// ************************************************************************** //

#if !BOOST_WORKAROUND(BOOST_MSVC, <1300)

namespace {

struct results_collector_impl {
    std::map<test_unit_id,test_results> m_results_store;
};

results_collector_impl& s_rc_impl() { static results_collector_impl the_inst; return the_inst; }

} // local namespace

#else

struct results_collector_impl {
    std::map<test_unit_id,test_results> m_results_store;
};

static results_collector_impl& s_rc_impl() { static results_collector_impl the_inst; return the_inst; }

#endif

//____________________________________________________________________________//

void
results_collector_t::test_start( counter_t )
{
    s_rc_impl().m_results_store.clear();
}

//____________________________________________________________________________//

void
results_collector_t::test_finish()
{
    // do nothing
}

//____________________________________________________________________________//

void
results_collector_t::test_aborted()
{
    // do nothing
}

//____________________________________________________________________________//

void
results_collector_t::test_unit_start( test_unit const& tu )
{
    // init test_results entry
    test_results& tr = s_rc_impl().m_results_store[tu.p_id];

    tr.clear();
    
    tr.p_expected_failures.value    = tu.p_expected_failures;
    tr.p_skipped.value              = false;
}

//____________________________________________________________________________//

class results_collect_helper : public test_tree_visitor {
public:
    explicit results_collect_helper( test_results& tr, test_unit const& ts ) : m_tr( tr ), m_ts( ts ) {}

    void    visit( test_case const& tc )
    {
        test_results const& tr = results_collector.results( tc.p_id );
        m_tr += tr;

        if( tr.passed() )
            m_tr.p_test_cases_passed.value++;
        else if( tr.p_skipped )
            m_tr.p_test_cases_skipped.value++;
        else {
            if( tr.p_aborted )
                m_tr.p_test_cases_aborted.value++;
            m_tr.p_test_cases_failed.value++;
        }
    }
    bool    test_suite_start( test_suite const& ts )
    {
        if( m_ts.p_id == ts.p_id )
            return true;
        else {
            m_tr += results_collector.results( ts.p_id );
            return false;
        }
    }

private:
    // Data members
    test_results&       m_tr;
    test_unit const&    m_ts;
};

//____________________________________________________________________________//

void
results_collector_t::test_unit_finish( test_unit const& tu, unsigned long )
{
    if( tu.p_type == tut_suite ) {
        results_collect_helper ch( s_rc_impl().m_results_store[tu.p_id], tu );

        traverse_test_tree( tu, ch );
    }
    else {
        test_results const& tr = s_rc_impl().m_results_store[tu.p_id];
        
        bool num_failures_match = tr.p_aborted || tr.p_assertions_failed >= tr.p_expected_failures;
        if( !num_failures_match )
            BOOST_TEST_MESSAGE( "Test case " << tu.p_name << " has fewer failures than expected" );

        bool check_any_assertions = tr.p_aborted || (tr.p_assertions_failed != 0) || (tr.p_assertions_passed != 0);
        if( !check_any_assertions )
            BOOST_TEST_MESSAGE( "Test case " << tu.p_name << " did not check any assertions" );
    }
}

//____________________________________________________________________________//

void
results_collector_t::test_unit_skipped( test_unit const& tu )
{
    if( tu.p_type == tut_suite ) {
        test_case_counter tcc;
        traverse_test_tree( tu, tcc );

        test_results& tr = s_rc_impl().m_results_store[tu.p_id];

        tr.clear();
    
        tr.p_skipped.value              = true;
        tr.p_test_cases_skipped.value   = tcc.p_count;
    }
}

//____________________________________________________________________________//

void
results_collector_t::assertion_result( bool passed )
{
    test_results& tr = s_rc_impl().m_results_store[framework::current_test_case().p_id];

    if( passed )
        tr.p_assertions_passed.value++;
    else
        tr.p_assertions_failed.value++;

    if( tr.p_assertions_failed == 1 )
        first_failed_assertion();
}

//____________________________________________________________________________//

void
results_collector_t::exception_caught( execution_exception const& )
{
    test_results& tr = s_rc_impl().m_results_store[framework::current_test_case().p_id];

    tr.p_assertions_failed.value++;
}

//____________________________________________________________________________//

void
results_collector_t::test_unit_aborted( test_unit const& tu )
{
    s_rc_impl().m_results_store[tu.p_id].p_aborted.value = true;
}

//____________________________________________________________________________//

test_results const&
results_collector_t::results( test_unit_id id ) const
{
    return s_rc_impl().m_results_store[id];
}

//____________________________________________________________________________//

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_RESULTS_COLLECTOR_IPP_021105GER
