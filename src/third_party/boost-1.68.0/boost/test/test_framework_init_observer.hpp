// (c) Copyright Raffi Enficiaud 2017.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// See http://www.boost.org/libs/test for the library home page.
//
/// @file
/// @brief Defines an observer that monitors the init of the unit test framework
// ***************************************************************************

#ifndef BOOST_TEST_FRAMEWORK_INIT_OBSERVER_HPP_071894GER
#define BOOST_TEST_FRAMEWORK_INIT_OBSERVER_HPP_071894GER

// Boost.Test
#include <boost/test/tree/observer.hpp>

#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/detail/fwd_decl.hpp>

#include <boost/test/utils/trivial_singleton.hpp>

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {
namespace unit_test {

// ************************************************************************** //
/// @brief Monitors the init of the framework
///
/// This class collects the state of the init/termination of the unit test framework.
///
/// @see boost::unit_test::test_observer
class BOOST_TEST_DECL framework_init_observer_t : public test_observer, public singleton<framework_init_observer_t> {
public:

    virtual void        test_start( counter_t );

    virtual void        assertion_result( unit_test::assertion_result );
    virtual void        exception_caught( execution_exception const& );
    virtual void        test_aborted();

    virtual int         priority() { return 0; }

    void                clear();

    /// Indicates if a failure has been recorded so far
    bool                has_failed( ) const;

private:
    BOOST_TEST_SINGLETON_CONS( framework_init_observer_t )
};

BOOST_TEST_SINGLETON_INST( framework_init_observer )

} // namespace unit_test
} // namespace boost

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_FRAMEWORK_INIT_OBSERVER_HPP_071894GER
