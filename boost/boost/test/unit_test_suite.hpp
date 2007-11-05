//  (C) Copyright Gennadiy Rozental 2001-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: unit_test_suite.hpp,v $
//
//  Version     : $Revision: 1.36 $
//
//  Description : defines Unit Test Framework public API
// ***************************************************************************

#ifndef BOOST_TEST_UNIT_TEST_SUITE_HPP_071894GER
#define BOOST_TEST_UNIT_TEST_SUITE_HPP_071894GER

// Boost.Test
#include <boost/test/unit_test_suite_impl.hpp>

//____________________________________________________________________________//

// ************************************************************************** //
// **************    Non-auto (explicit) test case interface   ************** //
// ************************************************************************** //

#define BOOST_TEST_CASE( function ) \
boost::unit_test::make_test_case( boost::unit_test::callback0<>(function), BOOST_TEST_STRINGIZE( function ) )
#define BOOST_CLASS_TEST_CASE( function, tc_instance ) \
boost::unit_test::make_test_case((function), BOOST_TEST_STRINGIZE( function ), tc_instance )

// ************************************************************************** //
// **************               BOOST_TEST_SUITE               ************** //
// ************************************************************************** //

#define BOOST_TEST_SUITE( testsuite_name ) \
( new boost::unit_test::test_suite( testsuite_name ) )

// ************************************************************************** //
// **************             BOOST_AUTO_TEST_SUITE            ************** //
// ************************************************************************** //

#define BOOST_AUTO_TEST_SUITE( suite_name )                             \
namespace suite_name {                                                  \
BOOST_AUTO_TC_REGISTRAR( suite_name )( BOOST_TEST_SUITE(                \
    BOOST_STRINGIZE( suite_name ) ) );                                  \
/**/

// ************************************************************************** //
// **************            BOOST_FIXTURE_TEST_SUITE          ************** //
// ************************************************************************** //

#define BOOST_FIXTURE_TEST_SUITE( suite_name, F )                       \
BOOST_AUTO_TEST_SUITE( suite_name )                                     \
typedef F BOOST_AUTO_TEST_CASE_FIXTURE;                                 \
/**/

// ************************************************************************** //
// **************           BOOST_AUTO_TEST_SUITE_END          ************** //
// ************************************************************************** //

#define BOOST_AUTO_TEST_SUITE_END()                                     \
BOOST_AUTO_TC_REGISTRAR( BOOST_JOIN( end_suite, __LINE__ ) )( 1 );      \
}                                                                       \
/**/

// ************************************************************************** //
// **************    BOOST_AUTO_TEST_CASE_EXPECTED_FAILURES    ************** //
// ************************************************************************** //

#define BOOST_AUTO_TEST_CASE_EXPECTED_FAILURES( test_name, n )          \
struct BOOST_AUTO_TC_UNIQUE_ID( test_name );                            \
namespace boost { namespace unit_test { namespace ut_detail {           \
                                                                        \
template<>                                                              \
struct auto_tc_exp_fail<BOOST_AUTO_TC_UNIQUE_ID( test_name ) > {        \
    enum { value = n };                                                 \
};                                                                      \
                                                                        \
}}}                                                                     \
/**/

// ************************************************************************** //
// **************            BOOST_FIXTURE_TEST_CASE           ************** //
// ************************************************************************** //

#define BOOST_FIXTURE_TEST_CASE( test_name, F )                         \
struct test_name : public F { void test_method(); };                    \
                                                                        \
static void BOOST_AUTO_TC_INVOKER( test_name )()                        \
{                                                                       \
    test_name t;                                                        \
    t.test_method();                                                    \
}                                                                       \
                                                                        \
struct BOOST_AUTO_TC_UNIQUE_ID( test_name ) {};                         \
                                                                        \
BOOST_AUTO_TC_REGISTRAR( test_name )(                                   \
    boost::unit_test::make_test_case(                                   \
        &BOOST_AUTO_TC_INVOKER( test_name ), #test_name ),              \
    boost::unit_test::ut_detail::auto_tc_exp_fail<                      \
        BOOST_AUTO_TC_UNIQUE_ID( test_name )>::value );                 \
                                                                        \
void test_name::test_method()                                           \
/**/

// ************************************************************************** //
// **************             BOOST_AUTO_TEST_CASE             ************** //
// ************************************************************************** //

#define BOOST_AUTO_TEST_CASE( test_name )                               \
BOOST_FIXTURE_TEST_CASE( test_name, BOOST_AUTO_TEST_CASE_FIXTURE )
/**/

// ************************************************************************** //
// **************        BOOST_AUTO_TEST_CASE_TEMPLATE         ************** //
// ************************************************************************** //

#define BOOST_AUTO_TEST_CASE_TEMPLATE( test_name, type_name, TL )       \
template<typename type_name>                                            \
struct test_name : public BOOST_AUTO_TEST_CASE_FIXTURE                  \
{ void test_method(); };                                                \
                                                                        \
struct BOOST_AUTO_TC_INVOKER( test_name ) {                             \
    template<typename TestType>                                         \
    static void run( boost::type<TestType>* = 0 )                       \
    {                                                                   \
        test_name<TestType> t;                                          \
        t.test_method();                                                \
    }                                                                   \
};                                                                      \
                                                                        \
BOOST_AUTO_TC_REGISTRAR( test_name )(                                   \
    boost::unit_test::ut_detail::template_test_case_gen<                \
        BOOST_AUTO_TC_INVOKER( test_name ),TL >(                        \
          BOOST_STRINGIZE( test_name ) ) );                             \
                                                                        \
template<typename type_name>                                            \
void test_name<type_name>::test_method()                                \
/**/

// ************************************************************************** //
// **************              BOOST_GLOBAL_FIXURE             ************** //
// ************************************************************************** //

#define BOOST_GLOBAL_FIXTURE( F ) \
static boost::unit_test::ut_detail::global_fixture_impl<F> BOOST_JOIN( gf_, F ) ; \
/**/

// ************************************************************************** //
// **************         BOOST_AUTO_TEST_CASE_FIXTURE         ************** //
// ************************************************************************** //

namespace boost { namespace unit_test { namespace ut_detail {

struct nil_t {};

} // namespace ut_detail
} // unit_test
} // namespace boost

// Intentionally is in global namespace, so that FIXURE_TEST_SUITE could reset it in user code.
typedef ::boost::unit_test::ut_detail::nil_t BOOST_AUTO_TEST_CASE_FIXTURE;

// ************************************************************************** //
// **************   Auto registration facility helper macros   ************** //
// ************************************************************************** //

#define BOOST_AUTO_TC_REGISTRAR( test_name )    \
static boost::unit_test::ut_detail::auto_test_unit_registrar BOOST_JOIN( test_name, _registrar )
#define BOOST_AUTO_TC_INVOKER( test_name )      BOOST_JOIN( test_name, _invoker )
#define BOOST_AUTO_TC_UNIQUE_ID( test_name )    BOOST_JOIN( test_name, _id )

// ************************************************************************** //
// **************                BOOST_TEST_MAIN               ************** //
// ************************************************************************** //

#if defined(BOOST_TEST_MAIN)

#ifdef BOOST_TEST_ALTERNATIVE_INIT_API
bool init_unit_test()                   {
#else
::boost::unit_test::test_suite*
init_unit_test_suite( int, char* [] )   {
#endif

#ifdef BOOST_TEST_MODULE
    using namespace ::boost::unit_test;
    assign_op( framework::master_test_suite().p_name.value, BOOST_TEST_STRINGIZE( BOOST_TEST_MODULE ).trim( "\"" ), 0 );
    
#endif

#ifdef BOOST_TEST_ALTERNATIVE_INIT_API
    return true;
}
#else
    return 0;
}
#endif

#endif

//____________________________________________________________________________//

// ***************************************************************************
//  Revision History :
//  
//  $Log: unit_test_suite.hpp,v $
//  Revision 1.36  2006/03/19 12:23:21  rogeeff
//  eliminate warning
//
//  Revision 1.35  2006/02/06 10:04:55  rogeeff
//  BOOST_TEST_MODULE - master test suite name
//
//  Revision 1.34  2006/01/28 07:02:57  rogeeff
//  allow multiple global fixtures
//
//  Revision 1.33  2005/12/14 05:24:55  rogeeff
//  dll support introduced
//  split into 2 files
//
// ***************************************************************************

#endif // BOOST_TEST_UNIT_TEST_SUITE_HPP_071894GER

