// (c) Copyright Raffi Enficiaud 2017.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// See http://www.boost.org/libs/test for the library home page.
//
//! @file
//! An observer for monitoring the success/failure of the other observers
// ***************************************************************************

#ifndef BOOST_TEST_FRAMEWORK_INIT_OBSERVER_IPP_021105GER
#define BOOST_TEST_FRAMEWORK_INIT_OBSERVER_IPP_021105GER

// Boost.Test
#include <boost/test/test_framework_init_observer.hpp>
#include <boost/test/framework.hpp>
#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {
namespace unit_test {


//____________________________________________________________________________//

// ************************************************************************** //
// **************           framework_init_observer_t          ************** //
// ************************************************************************** //

namespace {

struct test_init_observer_check {
    bool has_failure;

    void clear()
    {
      has_failure = false;
    }
};


test_init_observer_check& s_tioc_impl() { static test_init_observer_check the_inst; return the_inst; }

} // local namespace

void
framework_init_observer_t::clear()
{
    if(!framework::test_in_progress())
        s_tioc_impl().clear();
}

//____________________________________________________________________________//

void
framework_init_observer_t::test_start( counter_t )
{
    clear();
}

//____________________________________________________________________________//

void
framework_init_observer_t::assertion_result( unit_test::assertion_result ar )
{
    test_init_observer_check& tr = s_tioc_impl();
    switch( ar ) {
    case AR_TRIGGERED: break;
    case AR_PASSED: break;
    case AR_FAILED: tr.has_failure = true; break;
    default:
        break;
    }
}

//____________________________________________________________________________//

void
framework_init_observer_t::exception_caught( execution_exception const& )
{
    test_init_observer_check& tr = s_tioc_impl();
    tr.has_failure = true;
}

void
framework_init_observer_t::test_aborted()
{
    s_tioc_impl().has_failure = true;
}


//____________________________________________________________________________//

bool
framework_init_observer_t::has_failed() const
{
    return s_tioc_impl().has_failure;
}

//____________________________________________________________________________//

} // namespace unit_test
} // namespace boost

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_FRAMEWORK_INIT_OBSERVER_IPP_021105GER
