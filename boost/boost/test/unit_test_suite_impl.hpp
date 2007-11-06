//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: unit_test_suite_impl.hpp,v $
//
//  Version     : $Revision: 1.2 $
//
//  Description : defines test_unit, test_case, test_case_results, test_suite and test_tree_visitor
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_SUITE_IMPL_HPP_071894GER
#define BOOST_TEST_UNIT_TEST_SUITE_IMPL_HPP_071894GER

// Boost.Test
#include <boost/test/detail/config.hpp>
#include <boost/test/detail/global_typedef.hpp>
#include <boost/test/utils/class_properties.hpp>
#include <boost/test/utils/callback.hpp>
#include <boost/test/detail/fwd_decl.hpp>
#include <boost/test/detail/workaround.hpp>
#include <boost/test/test_observer.hpp>
#include <boost/test/framework.hpp>

// Boost
#include <boost/shared_ptr.hpp>

// STL
#include <string>   // for std::string
#include <list>     // for std::list
#include <vector>   // for std::vector

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

// ************************************************************************** //
// **************                   test_unit                  ************** //
// ************************************************************************** //

class BOOST_TEST_DECL test_unit {
public:
    enum { type = tut_any };

    // Constructor
    test_unit( const_string tu_name, test_unit_type t );

    // dependencies management
    void    depends_on( test_unit* tu );
    bool    check_dependencies() const;

    // Public r/o properties
    typedef BOOST_READONLY_PROPERTY(test_unit_id,(framework_impl))  id_t;
    typedef BOOST_READONLY_PROPERTY(test_unit_id,(test_suite))      parent_id_t;
    readonly_property<test_unit_type>   p_type;                 // type for this test unit
    readonly_property<const_string>     p_type_name;            // "case"/"suite"
    id_t                                p_id;                   // unique id for this test unit
    parent_id_t                         p_parent_id;            // parent test suite id

    // Public r/w properties
    readwrite_property<std::string>     p_name;                 // name for this test unit
    readwrite_property<unsigned>        p_timeout;              // timeout for the test unit execution 
    readwrite_property<counter_t>       p_expected_failures;    // number of expected failured in this test unit

private:
    // Data members
    std::list<test_unit_id>             m_dependencies;
};

// ************************************************************************** //
// **************              test_case_generator             ************** //
// ************************************************************************** //

class BOOST_TEST_DECL test_unit_generator {
public:
    virtual test_unit*  next() const = 0;

protected:
    BOOST_TEST_PROTECTED_VIRTUAL ~test_unit_generator() {}
};

// ************************************************************************** //
// **************                   test_case                  ************** //
// ************************************************************************** //

class BOOST_TEST_DECL test_case : public test_unit {
public:
    enum { type = tut_case };

    // Constructor
    test_case( const_string tc_name, callback0<> const& test_func );

    // Access methods
    callback0<> const&  test_func() const { return m_test_func; }

private:
    friend class framework_impl;
    ~test_case() {}

    // BOOST_MSVC <= 1200 have problems with callback as property
    // Data members
    callback0<> m_test_func;
};

// ************************************************************************** //
// **************                  test_suite                  ************** //
// ************************************************************************** //

class BOOST_TEST_DECL test_suite : public test_unit {
public:
    enum { type = tut_suite };

    // Constructor
    explicit    test_suite( const_string ts_name );

    // test case list management
    void        add( test_unit* tu, counter_t expected_failures = 0, unsigned timeout = 0 );
    void        add( test_unit_generator const& gen, unsigned timeout = 0 );

protected:
    friend BOOST_TEST_DECL void traverse_test_tree( test_suite const&, test_tree_visitor& );
    friend class framework_impl;
    virtual     ~test_suite() {}

private:
    // Data members
    std::vector<test_unit_id> m_members;
};

// ************************************************************************** //
// **************               master_test_suite              ************** //
// ************************************************************************** //

class BOOST_TEST_DECL master_test_suite_t : public test_suite {
public:
    master_test_suite_t() : test_suite( "Master Test Suite" )
    , argc( 0 )
    , argv( 0 )
    {}
    
    // Data members    
    int      argc;
    char**   argv;
};


// ************************************************************************** //
// **************               test_tree_visitor              ************** //
// ************************************************************************** //

class BOOST_TEST_DECL test_tree_visitor {
public:
    // test tree visitor interface
    virtual void    visit( test_case const& )               {}
    virtual bool    test_suite_start( test_suite const& )   { return true; }
    virtual void    test_suite_finish( test_suite const& )  {}

protected:
    BOOST_TEST_PROTECTED_VIRTUAL ~test_tree_visitor() {}
};

// ************************************************************************** //
// **************               traverse_test_tree             ************** //
// ************************************************************************** //

BOOST_TEST_DECL void    traverse_test_tree( test_case const&, test_tree_visitor& );
BOOST_TEST_DECL void    traverse_test_tree( test_suite const&, test_tree_visitor& );
BOOST_TEST_DECL void    traverse_test_tree( test_unit_id id, test_tree_visitor& );

//____________________________________________________________________________//

inline void
traverse_test_tree( test_unit const& tu, test_tree_visitor& V )
{
    if( tu.p_type == tut_case )
        traverse_test_tree( static_cast<test_case const&>( tu ), V );
    else
        traverse_test_tree( static_cast<test_suite const&>( tu ), V );
}

//____________________________________________________________________________//

// ************************************************************************** //
// **************                test_case_counter             ************** //
// ************************************************************************** //

struct test_case_counter : test_tree_visitor {
    test_case_counter() : m_count( 0 ) {}

    void        visit( test_case const& ) { m_count++; }

    counter_t   m_count;
};

// ************************************************************************** //
// **************               test_being_aborted             ************** //
// ************************************************************************** //

struct BOOST_TEST_DECL test_being_aborted {};

// ************************************************************************** //
// **************               object generators              ************** //
// ************************************************************************** //

namespace ut_detail {

BOOST_TEST_DECL std::string normalize_test_case_name( const_string tu_name );

template<typename UserTestCase>
struct user_tc_method_invoker {
    typedef void (UserTestCase::*test_method )();

    user_tc_method_invoker( shared_ptr<UserTestCase> inst, test_method test_method )
    : m_inst( inst ), m_test_method( test_method ) {}

    void operator()() { ((*m_inst).*m_test_method)(); }

    shared_ptr<UserTestCase> m_inst;
    test_method              m_test_method;
};

} // namespace ut_detail

//____________________________________________________________________________//

inline test_case*
make_test_case( callback0<> const& test_func, const_string tc_name )
{
    return new test_case( ut_detail::normalize_test_case_name( tc_name ), test_func );
}

//____________________________________________________________________________//

template<typename UserTestCase>
inline test_case*
make_test_case( void (UserTestCase::*test_method )(),
                  const_string tc_name,
                  boost::shared_ptr<UserTestCase> const& user_test_case )
{
    return new test_case( ut_detail::normalize_test_case_name( tc_name ), 
                          ut_detail::user_tc_method_invoker<UserTestCase>( user_test_case, test_method ) );
}

//____________________________________________________________________________//

// ************************************************************************** //
// **************           auto_test_unit_registrar           ************** //
// ************************************************************************** //

namespace ut_detail {

struct BOOST_TEST_DECL auto_test_unit_registrar
{
    // Constructor
    explicit    auto_test_unit_registrar( test_case* tc, counter_t exp_fail )
    {
        curr_ts_store().back()->add( tc, exp_fail );
    }
    explicit    auto_test_unit_registrar( test_suite* ts )
    {
        curr_ts_store().back()->add( ts );

        curr_ts_store().push_back( ts );
    }
    explicit    auto_test_unit_registrar( test_unit_generator const& tc_gen )
    {
        curr_ts_store().back()->add( tc_gen );
    }
    explicit    auto_test_unit_registrar( int )
    {
        if( curr_ts_store().size() > 1 )
            curr_ts_store().pop_back();
        // else report error
    }

private:
    static std::list<test_suite*>& curr_ts_store()
    {
        static std::list<test_suite*> inst( 1, &framework::master_test_suite() );
        return inst;
    }
};

//____________________________________________________________________________//

template<typename T>
struct auto_tc_exp_fail {
    enum { value = 0 };
};

//____________________________________________________________________________//

} // namespace ut_detail

// ************************************************************************** //
// **************                global_fixture                ************** //
// ************************************************************************** //

class BOOST_TEST_DECL global_fixture : public test_observer { 
public: 
    // Constructor
    global_fixture() { framework::register_observer( *this ); } 
}; 

//____________________________________________________________________________//

namespace ut_detail {

template<typename F> 
struct global_fixture_impl : public global_fixture {
    // Constructor
    global_fixture_impl(): m_fixure( 0 )    {}

    // test observer interface
    virtual void    test_start( counter_t ) { m_fixure = new F; }
    virtual void    test_finish()           { delete m_fixure; m_fixure = 0; } 
    virtual void    test_aborted()          { delete m_fixure; m_fixure = 0; } 

private:
    // Data members
    F*  m_fixure;
}; 

} // namespace ut_detail

} // unit_test

} // namespace boost

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: unit_test_suite_impl.hpp,v $
//  Revision 1.2  2006/01/28 07:09:34  rogeeff
//  tm->test_method
//
//  Revision 1.1  2005/12/14 05:24:55  rogeeff
//  dll support introduced
//  split into 2 files
//
// ***************************************************************************

#endif // BOOST_TEST_UNIT_TEST_SUITE_IMPL_HPP_071894GER

