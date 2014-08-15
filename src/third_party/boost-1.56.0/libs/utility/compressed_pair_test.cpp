//  boost::compressed_pair test program   
    
//  (C) Copyright John Maddock 2000. 
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).

// standalone test program for <boost/compressed_pair.hpp>
// Revised 03 Oct 2000: 
//    Enabled tests for VC6.

#include <iostream>
#include <typeinfo>
#include <cassert>

#include <boost/compressed_pair.hpp>
#include <boost/test/test_tools.hpp>

using namespace boost;

struct empty_UDT
{
   ~empty_UDT(){};
   empty_UDT& operator=(const empty_UDT&){ return *this; }
   bool operator==(const empty_UDT&)const
   { return true; }
};
struct empty_POD_UDT
{
   empty_POD_UDT& operator=(const empty_POD_UDT&){ return *this; }
   bool operator==(const empty_POD_UDT&)const
   { return true; }
};

struct non_empty1
{ 
   int i;
   non_empty1() : i(1){}
   non_empty1(int v) : i(v){}
   friend bool operator==(const non_empty1& a, const non_empty1& b)
   { return a.i == b.i; }
};

struct non_empty2
{ 
   int i;
   non_empty2() : i(3){}
   non_empty2(int v) : i(v){}
   friend bool operator==(const non_empty2& a, const non_empty2& b)
   { return a.i == b.i; }
};

#ifdef __GNUC__
using std::swap;
#endif

template <class T1, class T2>
struct compressed_pair_tester
{
   // define the types we need:
   typedef T1                                                 first_type;
   typedef T2                                                 second_type;
   typedef typename call_traits<first_type>::param_type       first_param_type;
   typedef typename call_traits<second_type>::param_type      second_param_type;
   // define our test proc:
   static void test(first_param_type p1, second_param_type p2, first_param_type p3, second_param_type p4);
};

template <class T1, class T2>
void compressed_pair_tester<T1, T2>::test(first_param_type p1, second_param_type p2, first_param_type p3, second_param_type p4)
{
#ifndef __GNUC__
   // gcc 2.90 can't cope with function scope using
   // declarations, and generates an internal compiler error...
   using std::swap;
#endif
   // default construct:
   boost::compressed_pair<T1,T2> cp1;
   // first param construct:
   boost::compressed_pair<T1,T2> cp2(p1);
   cp2.second() = p2;
   BOOST_CHECK(cp2.first() == p1);
   BOOST_CHECK(cp2.second() == p2);
   // second param construct:
   boost::compressed_pair<T1,T2> cp3(p2);
   cp3.first() = p1;
   BOOST_CHECK(cp3.second() == p2);
   BOOST_CHECK(cp3.first() == p1);
   // both param construct:
   boost::compressed_pair<T1,T2> cp4(p1, p2);
   BOOST_CHECK(cp4.first() == p1);
   BOOST_CHECK(cp4.second() == p2);
   boost::compressed_pair<T1,T2> cp5(p3, p4);
   BOOST_CHECK(cp5.first() == p3);
   BOOST_CHECK(cp5.second() == p4);
   // check const members:
   const boost::compressed_pair<T1,T2>& cpr1 = cp4;
   BOOST_CHECK(cpr1.first() == p1);
   BOOST_CHECK(cpr1.second() == p2);

   // copy construct:
   boost::compressed_pair<T1,T2> cp6(cp4);
   BOOST_CHECK(cp6.first() == p1);
   BOOST_CHECK(cp6.second() == p2);
   // assignment:
   cp1 = cp4;
   BOOST_CHECK(cp1.first() == p1);
   BOOST_CHECK(cp1.second() == p2);
   cp1 = cp5;
   BOOST_CHECK(cp1.first() == p3);
   BOOST_CHECK(cp1.second() == p4);
   // swap:
   cp4.swap(cp5);
   BOOST_CHECK(cp4.first() == p3);
   BOOST_CHECK(cp4.second() == p4);
   BOOST_CHECK(cp5.first() == p1);
   BOOST_CHECK(cp5.second() == p2);
   swap(cp4,cp5);
   BOOST_CHECK(cp4.first() == p1);
   BOOST_CHECK(cp4.second() == p2);
   BOOST_CHECK(cp5.first() == p3);
   BOOST_CHECK(cp5.second() == p4);
}

//
// tests for case where one or both 
// parameters are reference types:
//
template <class T1, class T2>
struct compressed_pair_reference_tester
{
   // define the types we need:
   typedef T1                                                 first_type;
   typedef T2                                                 second_type;
   typedef typename call_traits<first_type>::param_type       first_param_type;
   typedef typename call_traits<second_type>::param_type      second_param_type;
   // define our test proc:
   static void test(first_param_type p1, second_param_type p2, first_param_type p3, second_param_type p4);
};

template <class T1, class T2>
void compressed_pair_reference_tester<T1, T2>::test(first_param_type p1, second_param_type p2, first_param_type p3, second_param_type p4)
{
#ifndef __GNUC__
   // gcc 2.90 can't cope with function scope using
   // declarations, and generates an internal compiler error...
   using std::swap;
#endif
   // both param construct:
   boost::compressed_pair<T1,T2> cp4(p1, p2);
   BOOST_CHECK(cp4.first() == p1);
   BOOST_CHECK(cp4.second() == p2);
   boost::compressed_pair<T1,T2> cp5(p3, p4);
   BOOST_CHECK(cp5.first() == p3);
   BOOST_CHECK(cp5.second() == p4);
   // check const members:
   const boost::compressed_pair<T1,T2>& cpr1 = cp4;
   BOOST_CHECK(cpr1.first() == p1);
   BOOST_CHECK(cpr1.second() == p2);

   // copy construct:
   boost::compressed_pair<T1,T2> cp6(cp4);
   BOOST_CHECK(cp6.first() == p1);
   BOOST_CHECK(cp6.second() == p2);
   // assignment:
   // VC6 bug:
   // When second() is an empty class, VC6 performs the
   // assignment by doing a memcpy - even though the empty
   // class is really a zero sized base class, the result
   // is that the memory of first() gets trampled over.
   // Similar arguments apply to the case that first() is 
   // an empty base class.
   // Strangely the problem is dependent upon the compiler
   // settings - some generate the problem others do not.
   cp4.first() = p3;
   cp4.second() = p4;
   BOOST_CHECK(cp4.first() == p3);
   BOOST_CHECK(cp4.second() == p4);
}
//
// supplimentary tests for case where first arg only is a reference type:
//
template <class T1, class T2>
struct compressed_pair_reference1_tester
{
   // define the types we need:
   typedef T1                                                 first_type;
   typedef T2                                                 second_type;
   typedef typename call_traits<first_type>::param_type       first_param_type;
   typedef typename call_traits<second_type>::param_type      second_param_type;
   // define our test proc:
   static void test(first_param_type p1, second_param_type p2, first_param_type p3, second_param_type p4);
};

template <class T1, class T2>
void compressed_pair_reference1_tester<T1, T2>::test(first_param_type p1, second_param_type p2, first_param_type, second_param_type)
{
#ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
   // first param construct:
   boost::compressed_pair<T1,T2> cp2(p1);
   cp2.second() = p2;
   BOOST_CHECK(cp2.first() == p1);
   BOOST_CHECK(cp2.second() == p2);
#endif
}
//
// supplimentary tests for case where second arg only is a reference type:
//
template <class T1, class T2>
struct compressed_pair_reference2_tester
{
   // define the types we need:
   typedef T1                                                 first_type;
   typedef T2                                                 second_type;
   typedef typename call_traits<first_type>::param_type       first_param_type;
   typedef typename call_traits<second_type>::param_type      second_param_type;
   // define our test proc:
   static void test(first_param_type p1, second_param_type p2, first_param_type p3, second_param_type p4);
};

template <class T1, class T2>
void compressed_pair_reference2_tester<T1, T2>::test(first_param_type p1, second_param_type p2, first_param_type, second_param_type)
{
#ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
   // second param construct:
   boost::compressed_pair<T1,T2> cp3(p2);
   cp3.first() = p1;
   BOOST_CHECK(cp3.second() == p2);
   BOOST_CHECK(cp3.first() == p1);
#endif
}

//
// tests for where one or the other parameter is an array:
//
template <class T1, class T2>
struct compressed_pair_array1_tester
{
   // define the types we need:
   typedef T1                                                 first_type;
   typedef T2                                                 second_type;
   typedef typename call_traits<first_type>::param_type       first_param_type;
   typedef typename call_traits<second_type>::param_type      second_param_type;
   // define our test proc:
   static void test(first_param_type p1, second_param_type p2, first_param_type p3, second_param_type p4);
};

template <class T1, class T2>
void compressed_pair_array1_tester<T1, T2>::test(first_param_type p1, second_param_type p2, first_param_type, second_param_type)
{
  // default construct:
   boost::compressed_pair<T1,T2> cp1;
   // second param construct:
   boost::compressed_pair<T1,T2> cp3(p2);
   cp3.first()[0] = p1[0];
   BOOST_CHECK(cp3.second() == p2);
   BOOST_CHECK(cp3.first()[0] == p1[0]);
   // check const members:
   const boost::compressed_pair<T1,T2>& cpr1 = cp3;
   BOOST_CHECK(cpr1.first()[0] == p1[0]);
   BOOST_CHECK(cpr1.second() == p2);

   BOOST_CHECK(sizeof(T1) == sizeof(cp1.first()));
}

template <class T1, class T2>
struct compressed_pair_array2_tester
{
   // define the types we need:
   typedef T1                                                 first_type;
   typedef T2                                                 second_type;
   typedef typename call_traits<first_type>::param_type       first_param_type;
   typedef typename call_traits<second_type>::param_type      second_param_type;
   // define our test proc:
   static void test(first_param_type p1, second_param_type p2, first_param_type p3, second_param_type p4);
};

template <class T1, class T2>
void compressed_pair_array2_tester<T1, T2>::test(first_param_type p1, second_param_type p2, first_param_type, second_param_type)
{
   // default construct:
   boost::compressed_pair<T1,T2> cp1;
   // first param construct:
   boost::compressed_pair<T1,T2> cp2(p1);
   cp2.second()[0] = p2[0];
   BOOST_CHECK(cp2.first() == p1);
   BOOST_CHECK(cp2.second()[0] == p2[0]);
   // check const members:
   const boost::compressed_pair<T1,T2>& cpr1 = cp2;
   BOOST_CHECK(cpr1.first() == p1);
   BOOST_CHECK(cpr1.second()[0] == p2[0]);

   BOOST_CHECK(sizeof(T2) == sizeof(cp1.second()));
}

template <class T1, class T2>
struct compressed_pair_array_tester
{
   // define the types we need:
   typedef T1                                                 first_type;
   typedef T2                                                 second_type;
   typedef typename call_traits<first_type>::param_type       first_param_type;
   typedef typename call_traits<second_type>::param_type      second_param_type;
   // define our test proc:
   static void test(first_param_type p1, second_param_type p2, first_param_type p3, second_param_type p4);
};

template <class T1, class T2>
void compressed_pair_array_tester<T1, T2>::test(first_param_type p1, second_param_type p2, first_param_type, second_param_type)
{
   // default construct:
   boost::compressed_pair<T1,T2> cp1;
   cp1.first()[0] = p1[0];
   cp1.second()[0] = p2[0];
   BOOST_CHECK(cp1.first()[0] == p1[0]);
   BOOST_CHECK(cp1.second()[0] == p2[0]);
   // check const members:
   const boost::compressed_pair<T1,T2>& cpr1 = cp1;
   BOOST_CHECK(cpr1.first()[0] == p1[0]);
   BOOST_CHECK(cpr1.second()[0] == p2[0]);

   BOOST_CHECK(sizeof(T1) == sizeof(cp1.first()));
   BOOST_CHECK(sizeof(T2) == sizeof(cp1.second()));
}

int test_main(int, char *[])
{
   // declare some variables to pass to the tester:
   non_empty1 ne1(2);
   non_empty1 ne2(3);
   non_empty2 ne3(4);
   non_empty2 ne4(5);
   empty_POD_UDT  e1;
   empty_UDT      e2;

   // T1 != T2, both non-empty
   compressed_pair_tester<non_empty1,non_empty2>::test(ne1, ne3, ne2, ne4);
   // T1 != T2, T2 empty
   compressed_pair_tester<non_empty1,empty_POD_UDT>::test(ne1, e1, ne2, e1);
   // T1 != T2, T1 empty
   compressed_pair_tester<empty_POD_UDT,non_empty2>::test(e1, ne3, e1, ne4);
   // T1 != T2, both empty
   compressed_pair_tester<empty_POD_UDT,empty_UDT>::test(e1, e2, e1, e2);
   // T1 == T2, both non-empty
   compressed_pair_tester<non_empty1,non_empty1>::test(ne1, ne1, ne2, ne2);
   // T1 == T2, both empty
   compressed_pair_tester<empty_UDT,empty_UDT>::test(e2, e2, e2, e2);


   // test references:

   // T1 != T2, both non-empty
   compressed_pair_reference_tester<non_empty1&,non_empty2>::test(ne1, ne3, ne2, ne4);
   compressed_pair_reference_tester<non_empty1,non_empty2&>::test(ne1, ne3, ne2, ne4);
   compressed_pair_reference1_tester<non_empty1&,non_empty2>::test(ne1, ne3, ne2, ne4);
   compressed_pair_reference2_tester<non_empty1,non_empty2&>::test(ne1, ne3, ne2, ne4);
   // T1 != T2, T2 empty
   compressed_pair_reference_tester<non_empty1&,empty_POD_UDT>::test(ne1, e1, ne2, e1);
   compressed_pair_reference1_tester<non_empty1&,empty_POD_UDT>::test(ne1, e1, ne2, e1);
   // T1 != T2, T1 empty
   compressed_pair_reference_tester<empty_POD_UDT,non_empty2&>::test(e1, ne3, e1, ne4);
   compressed_pair_reference2_tester<empty_POD_UDT,non_empty2&>::test(e1, ne3, e1, ne4);
   // T1 == T2, both non-empty
   compressed_pair_reference_tester<non_empty1&,non_empty1&>::test(ne1, ne1, ne2, ne2);

   // tests arrays:
   non_empty1 nea1[2];
   non_empty1 nea2[2];
   non_empty2 nea3[2];
   non_empty2 nea4[2];
   nea1[0] = non_empty1(5);
   nea2[0] = non_empty1(6);
   nea3[0] = non_empty2(7);
   nea4[0] = non_empty2(8);
   
   // T1 != T2, both non-empty
   compressed_pair_array1_tester<non_empty1[2],non_empty2>::test(nea1, ne3, nea2, ne4);
   compressed_pair_array2_tester<non_empty1,non_empty2[2]>::test(ne1, nea3, ne2, nea4);
   compressed_pair_array_tester<non_empty1[2],non_empty2[2]>::test(nea1, nea3, nea2, nea4);
   // T1 != T2, T2 empty
   compressed_pair_array1_tester<non_empty1[2],empty_POD_UDT>::test(nea1, e1, nea2, e1);
   // T1 != T2, T1 empty
   compressed_pair_array2_tester<empty_POD_UDT,non_empty2[2]>::test(e1, nea3, e1, nea4);
   // T1 == T2, both non-empty
   compressed_pair_array_tester<non_empty1[2],non_empty1[2]>::test(nea1, nea1, nea2, nea2);
   return 0;
}


unsigned int expected_failures = 0;





