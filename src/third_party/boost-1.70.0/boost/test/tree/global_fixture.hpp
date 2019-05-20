//  (C) Copyright Gennadiy Rozental 2001.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
/// @file
/// Defines global_fixture
// ***************************************************************************

#ifndef BOOST_TEST_TREE_GLOBAL_FIXTURE_HPP_091911GER
#define BOOST_TEST_TREE_GLOBAL_FIXTURE_HPP_091911GER

// Boost.Test
#include <boost/test/detail/config.hpp>
#include <boost/test/detail/global_typedef.hpp>

#include <boost/test/tree/observer.hpp>
#include <boost/test/tree/fixture.hpp>

#include <boost/test/detail/suppress_warnings.hpp>


//____________________________________________________________________________//

namespace boost {
namespace unit_test {

// ************************************************************************** //
// **************             global_configuration             ************** //
// ************************************************************************** //

class BOOST_TEST_DECL global_configuration : public test_observer {

public:
    // Constructor
    global_configuration();

    /// Unregisters the global fixture from the framework
    ///
    /// This is called by the framework at shutdown time
    void unregister_from_framework();

    // Dtor
    virtual ~global_configuration();

    // Happens after the framework global observer init has been done
    virtual int     priority() { return 1; }

private:
    bool registered;
};



// ************************************************************************** //
// **************                global_fixture                ************** //
// ************************************************************************** //

class BOOST_TEST_DECL global_fixture : public test_unit_fixture {

public:
    // Constructor
    global_fixture();

    /// Unregisters the global fixture from the framework
    ///
    /// This is called by the framework at shutdown time
    void unregister_from_framework();

    // Dtor
    virtual ~global_fixture();

private:
    bool registered;
};

//____________________________________________________________________________//

namespace ut_detail {

template<typename F>
struct global_configuration_impl : public global_configuration {
    // Constructor
    global_configuration_impl() : m_configuration_observer( 0 )  {
    }

    // test observer interface
    virtual void    test_start( counter_t ) {
        m_configuration_observer = new F;
    }

    // test observer interface
    virtual void    test_finish()           {
        if(m_configuration_observer) {
            delete m_configuration_observer;
            m_configuration_observer = 0;
        }
    }
private:
    // Data members
    F*  m_configuration_observer;
};

template<typename F>
struct global_fixture_impl : public global_fixture {
    // Constructor
    global_fixture_impl() : m_fixture( 0 )  {
    }

    // test fixture interface
    virtual void setup()                    {
        m_fixture = new F;
        setup_conditional(*m_fixture);
    }

    // test fixture interface
    virtual void teardown()                 {
        if(m_fixture) {
            teardown_conditional(*m_fixture);
        }
        delete m_fixture;
        m_fixture = 0;
    }

private:
    // Data members
    F*  m_fixture;
};

} // namespace ut_detail
} // namespace unit_test
} // namespace boost

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_TEST_TREE_GLOBAL_FIXTURE_HPP_091911GER

