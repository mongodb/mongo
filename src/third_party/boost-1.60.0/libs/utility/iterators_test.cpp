//  Demonstrate and test boost/operators.hpp on std::iterators  --------------//

//  (C) Copyright Jeremy Siek 1999.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version including documentation.

//  Revision History
//  29 May 01 Factored implementation, added comparison tests, use Test Tools
//            library (Daryle Walker)
//  12 Dec 99 Initial version with iterator operators (Jeremy Siek)

#define  BOOST_INCLUDE_MAIN
#include <boost/test/test_tools.hpp>  // for main

#include <boost/config.hpp>     // for BOOST_STATIC_CONSTANT
#include <boost/cstdlib.hpp>    // for boost::exit_success
#include <boost/operators.hpp>  // for boost::random_access_iterator_helper

#include <cstddef>    // for std::ptrdiff_t, std::size_t
#include <cstring>    // for std::strcmp
#include <iostream>   // for std::cout (std::endl, ends, and flush indirectly)
#include <string>     // for std::string
#include <sstream>    // for std::stringstream

# ifdef BOOST_NO_STDC_NAMESPACE
    namespace std { using ::strcmp; }
# endif


// Iterator test class
template <class T, class R, class P>
struct test_iter
  : public boost::random_access_iterator_helper<
     test_iter<T,R,P>, T, std::ptrdiff_t, P, R>
{
  typedef test_iter self;
  typedef R Reference;
  typedef std::ptrdiff_t Distance;

public:
  explicit test_iter(T* i =0) : _i(i) { }
  test_iter(const self& x) : _i(x._i) { }
  self& operator=(const self& x) { _i = x._i; return *this; }
  Reference operator*() const { return *_i; }
  self& operator++() { ++_i; return *this; }
  self& operator--() { --_i; return *this; }
  self& operator+=(Distance n) { _i += n; return *this; }
  self& operator-=(Distance n) { _i -= n; return *this; }
  bool operator==(const self& x) const { return _i == x._i; }
  bool operator<(const self& x) const { return _i < x._i; }
  friend Distance operator-(const self& x, const self& y) {
    return x._i - y._i; 
  }
protected:
  P _i;
};

// Iterator operator testing classes
class test_opr_base
{
protected:
    // Test data and types
    BOOST_STATIC_CONSTANT( std::size_t, fruit_length = 6u );

    typedef std::string  fruit_array_type[ fruit_length ];

    static  fruit_array_type    fruit;

};  // test_opr_base

#ifndef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
//  A definition is required even for integral static constants
const std::size_t test_opr_base::fruit_length;
#endif

template <typename T, typename R = T&, typename P = T*>
class test_opr
    : public test_opr_base
{
    typedef test_opr<T, R, P>  self_type;

public:
    // Types
    typedef T  value_type;
    typedef R  reference;
    typedef P  pointer;

    typedef test_iter<T, R, P>  iter_type;

    // Test controller
    static  void  master_test( char const name[] );

private:
    // Test data
    static iter_type const  fruit_begin;
    static iter_type const  fruit_end;

    // Test parts
    static  void  post_increment_test();
    static  void  post_decrement_test();
    static  void  indirect_referral_test();
    static  void  offset_addition_test();
    static  void  reverse_offset_addition_test();
    static  void  offset_subtraction_test();
    static  void  comparison_test();
    static  void  indexing_test();

};  // test_opr


// Class-static data definitions
test_opr_base::fruit_array_type
 test_opr_base::fruit = { "apple", "orange", "pear", "peach", "grape", "plum" };

template <typename T, typename R, typename P>
  typename test_opr<T, R, P>::iter_type const
 test_opr<T, R, P>::fruit_begin = test_iter<T,R,P>( fruit );

template <typename T, typename R, typename P>
typename test_opr<T, R, P>::iter_type const
 test_opr<T, R, P>::fruit_end = test_iter<T,R,P>( fruit + fruit_length );


// Main testing function
int
test_main( int , char * [] )
{
    using std::string;

    typedef test_opr<string, string &, string *>              test1_type;
    typedef test_opr<string, string const &, string const *>  test2_type;

    test1_type::master_test( "non-const string" );
    test2_type::master_test( "const string" );

    return boost::exit_success;
}

// Tests for all of the operators added by random_access_iterator_helper
template <typename T, typename R, typename P>
void
test_opr<T, R, P>::master_test
(
    char const  name[]
)
{
    std::cout << "Doing test run for " << name << '.' << std::endl;

    post_increment_test();
    post_decrement_test();
    indirect_referral_test();
    offset_addition_test();
    reverse_offset_addition_test();
    offset_subtraction_test();
    comparison_test();
    indexing_test();
}

// Test post-increment
template <typename T, typename R, typename P>
void
test_opr<T, R, P>::post_increment_test
(
)
{
    std::cout << "\tDoing post-increment test." << std::endl;

    std::stringstream oss;
    for ( iter_type i = fruit_begin ; i != fruit_end ; )
    {
        oss << *i++ << ' ';
    }

    BOOST_CHECK( oss.str() == "apple orange pear peach grape plum ");
}

// Test post-decrement
template <typename T, typename R, typename P>
void
test_opr<T, R, P>::post_decrement_test
(
)
{
    std::cout << "\tDoing post-decrement test." << std::endl;

    std::stringstream oss;
    for ( iter_type i = fruit_end ; i != fruit_begin ; )
    {
        i--;
        oss << *i << ' ';
    }

    BOOST_CHECK( oss.str() == "plum grape peach pear orange apple ");
}

// Test indirect structure referral
template <typename T, typename R, typename P>
void
test_opr<T, R, P>::indirect_referral_test
(
)
{
    std::cout << "\tDoing indirect reference test." << std::endl;

    std::stringstream oss;
    for ( iter_type i = fruit_begin ; i != fruit_end ; ++i )
    {
        oss << i->size() << ' ';
    }

    BOOST_CHECK( oss.str() == "5 6 4 5 5 4 ");
}

// Test offset addition
template <typename T, typename R, typename P>
void
test_opr<T, R, P>::offset_addition_test
(
)
{
    std::cout << "\tDoing offset addition test." << std::endl;

    std::ptrdiff_t const  two = 2;
    std::stringstream oss;
    for ( iter_type i = fruit_begin ; i != fruit_end ; i = i + two )
    {
        oss << *i << ' ';
    }

    BOOST_CHECK( oss.str() == "apple pear grape ");
}

// Test offset addition, in reverse order
template <typename T, typename R, typename P>
void
test_opr<T, R, P>::reverse_offset_addition_test
(
)
{
    std::cout << "\tDoing reverse offset addition test." << std::endl;

    std::ptrdiff_t const  two = 2;
    std::stringstream oss;
    for ( iter_type i = fruit_begin ; i != fruit_end ; i = two + i )
    {
        oss << *i << ' ';
    }

    BOOST_CHECK( oss.str() == "apple pear grape ");
}

// Test offset subtraction
template <typename T, typename R, typename P>
void
test_opr<T, R, P>::offset_subtraction_test
(
)
{
    std::cout << "\tDoing offset subtraction test." << std::endl;

    std::ptrdiff_t const  two = 2;
    std::stringstream oss;
    for ( iter_type i = fruit_end ; fruit_begin < i ; )
    {
        i = i - two;
        if ( (fruit_begin < i) || (fruit_begin == i) )
        {
            oss << *i << ' ';
        }
    }

    BOOST_CHECK( oss.str() == "grape pear apple ");
}

// Test comparisons
template <typename T, typename R, typename P>
void
test_opr<T, R, P>::comparison_test
(
)
{
    using std::cout;
    using std::ptrdiff_t;

    cout << "\tDoing comparison tests.\n\t\tPass:";

    for ( iter_type i = fruit_begin ; i != fruit_end ; ++i )
    {
        ptrdiff_t const  i_offset = i - fruit_begin;

        cout << ' ' << *i << std::flush;
        for ( iter_type j = fruit_begin ; j != fruit_end ; ++j )
        {
            ptrdiff_t const  j_offset = j - fruit_begin;

            BOOST_CHECK( (i != j) == (i_offset != j_offset) );
            BOOST_CHECK( (i > j) == (i_offset > j_offset) );
            BOOST_CHECK( (i <= j) == (i_offset <= j_offset) );
            BOOST_CHECK( (i >= j) == (i_offset >= j_offset) );
        }
    }
    cout << std::endl;
}

// Test indexing
template <typename T, typename R, typename P>
void
test_opr<T, R, P>::indexing_test
(
)
{
    std::cout << "\tDoing indexing test." << std::endl;

    std::stringstream oss;
    for ( std::size_t k = 0u ; k < fruit_length ; ++k )
    {
        oss << fruit_begin[ k ] << ' ';
    }

    BOOST_CHECK( oss.str() == "apple orange pear peach grape plum ");
}
