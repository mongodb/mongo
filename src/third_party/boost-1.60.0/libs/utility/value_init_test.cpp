// Copyright 2002-2008, Fernando Luis Cacciola Carballal.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// Test program for "boost/utility/value_init.hpp"
//
// 21 Ago 2002 (Created) Fernando Cacciola
// 15 Jan 2008 (Added tests regarding compiler issues) Fernando Cacciola, Niels Dekker
// 23 May 2008 (Added tests regarding initialized_value) Niels Dekker
// 21 Ago 2008 (Added swap test) Niels Dekker

#include <cstring>  // For memcmp.
#include <iostream>
#include <string>

#include "boost/utility/value_init.hpp"
#include <boost/shared_ptr.hpp>

#ifdef __BORLANDC__
#pragma hdrstop
#endif

#include <boost/detail/lightweight_test.hpp>

//
// Sample POD type
//
struct POD
{
  POD () : f(0), c(0), i(0){}

  POD ( char c_, int i_, float f_ ) : f(f_), c(c_), i(i_) {}

  friend std::ostream& operator << ( std::ostream& os, POD const& pod )
    { return os << '(' << pod.c << ',' << pod.i << ',' << pod.f << ')' ; }

  friend bool operator == ( POD const& lhs, POD const& rhs )
    { return lhs.f == rhs.f && lhs.c == rhs.c && lhs.i == rhs.i ; }

  float f;
  char  c;
  int   i;
} ;

//
// Sample non POD type
//
struct NonPODBase
{
  virtual ~NonPODBase() {}
} ;
struct NonPOD : NonPODBase
{
  NonPOD () : id() {}
  explicit NonPOD ( std::string const& id_) : id(id_) {}

  friend std::ostream& operator << ( std::ostream& os, NonPOD const& npod )
    { return os << '(' << npod.id << ')' ; }

  friend bool operator == ( NonPOD const& lhs, NonPOD const& rhs )
    { return lhs.id == rhs.id ; }

  std::string id ;
} ;

//
// Sample aggregate POD struct type
// Some compilers do not correctly value-initialize such a struct, for example:
// Borland C++ Report #51854, "Value-initialization: POD struct should be zero-initialized "
// http://qc.codegear.com/wc/qcmain.aspx?d=51854
//
struct AggregatePODStruct
{
  float f;
  char  c;
  int   i;
};

bool operator == ( AggregatePODStruct const& lhs, AggregatePODStruct const& rhs )
{ return lhs.f == rhs.f && lhs.c == rhs.c && lhs.i == rhs.i ; }

//
// An aggregate struct that contains an std::string and an int.
// Pavel Kuznetsov (MetaCommunications Engineering) used a struct like
// this to reproduce the Microsoft Visual C++ compiler bug, reported as
// Feedback ID 100744, "Value-initialization in new-expression"
// https://connect.microsoft.com/VisualStudio/feedback/ViewFeedback.aspx?FeedbackID=100744
//
struct StringAndInt
{
  std::string s;
  int i;
};

bool operator == ( StringAndInt const& lhs, StringAndInt const& rhs )
{ return lhs.s == rhs.s && lhs.i == rhs.i ; }


//
// A struct that has an explicit (user defined) destructor.
// Some compilers do not correctly value-initialize such a struct, for example:
// Microsoft Visual C++, Feedback ID 100744, "Value-initialization in new-expression"
// https://connect.microsoft.com/VisualStudio/feedback/ViewFeedback.aspx?FeedbackID=100744
//
struct StructWithDestructor
{
  int i;
  ~StructWithDestructor() {}
};

bool operator == ( StructWithDestructor const& lhs, StructWithDestructor const& rhs )
{ return lhs.i == rhs.i ; }


//
// A struct that has a virtual function.
// Some compilers do not correctly value-initialize such a struct either, for example:
// Microsoft Visual C++, Feedback ID 100744, "Value-initialization in new-expression"
// https://connect.microsoft.com/VisualStudio/feedback/ViewFeedback.aspx?FeedbackID=100744
//
struct StructWithVirtualFunction
{
  int i;
  virtual void VirtualFunction(); 
};

void StructWithVirtualFunction::VirtualFunction()
{
}

bool operator == ( StructWithVirtualFunction const& lhs, StructWithVirtualFunction const& rhs )
{ return lhs.i == rhs.i ; }


//
// A struct that is derived from an aggregate POD struct.
// Some compilers do not correctly value-initialize such a struct, for example:
// GCC Bugzilla Bug 30111,  "Value-initialization of POD base class doesn't initialize members",
// reported by Jonathan Wakely, http://gcc.gnu.org/bugzilla/show_bug.cgi?id=30111
//
struct DerivedFromAggregatePODStruct : AggregatePODStruct
{
  DerivedFromAggregatePODStruct() : AggregatePODStruct() {}
};

//
// A struct that wraps an aggregate POD struct as data member.
//
struct AggregatePODStructWrapper
{
  AggregatePODStructWrapper() : dataMember() {}
  AggregatePODStruct dataMember;
};

bool operator == ( AggregatePODStructWrapper const& lhs, AggregatePODStructWrapper const& rhs )
{ return lhs.dataMember == rhs.dataMember ; }

typedef unsigned char ArrayOfBytes[256];


//
// A struct that allows testing whether the appropriate copy functions are called.
//
struct CopyFunctionCallTester
{
  bool is_copy_constructed;
  bool is_assignment_called;

  CopyFunctionCallTester()
  : is_copy_constructed(false), is_assignment_called(false) {}

  CopyFunctionCallTester(const CopyFunctionCallTester & )
  : is_copy_constructed(true), is_assignment_called(false) {}

  CopyFunctionCallTester & operator=(const CopyFunctionCallTester & )
  {
    is_assignment_called = true ;
    return *this ;
  }
};


//
// A struct that allows testing whether its customized swap function is called.
//
struct SwapFunctionCallTester
{
  bool is_custom_swap_called;
  int data;

  SwapFunctionCallTester()
  : is_custom_swap_called(false), data(0) {}

  SwapFunctionCallTester(const SwapFunctionCallTester & arg)
  : is_custom_swap_called(false), data(arg.data) {}

  void swap(SwapFunctionCallTester & arg)
  {
    std::swap(data, arg.data);
    is_custom_swap_called = true;
    arg.is_custom_swap_called = true;
  }
};

void swap(SwapFunctionCallTester & lhs, SwapFunctionCallTester & rhs)
{
  lhs.swap(rhs);
}



template<class T>
void check_initialized_value ( T const& y )
{
  T initializedValue = boost::initialized_value ;
  BOOST_TEST ( y == initializedValue ) ;
}

#ifdef  __BORLANDC__
#if __BORLANDC__ == 0x582
void check_initialized_value( NonPOD const& )
{
  // The initialized_value check is skipped for Borland 5.82
  // and this type (NonPOD), because the following statement
  // won't compile on this particular compiler version:
  //   NonPOD initializedValue = boost::initialized_value() ;
  //
  // This is caused by a compiler bug, that is fixed with a newer version
  // of the Borland compiler.  The Release Notes for Delphi(R) 2007 for
  // Win32(R) and C++Builder(R) 2007 (http://dn.codegear.com/article/36575)
  // say about similar statements:
  //   both of these statements now compile but under 5.82 got the error:
  //   Error E2015: Ambiguity between 'V::V(const A &)' and 'V::V(const V &)'
}
#endif
#endif

//
// This test function tests boost::value_initialized<T> for a specific type T.
// The first argument (y) is assumed have the value of a value-initialized object.
// Returns true on success.
//
template<class T>
bool test ( T const& y, T const& z )
{
  const int errors_before_test = boost::detail::test_errors();

  check_initialized_value(y);

  boost::value_initialized<T> x ;
  BOOST_TEST ( y == x ) ;
  BOOST_TEST ( y == boost::get(x) ) ;

  static_cast<T&>(x) = z ;
  boost::get(x) = z ;
  BOOST_TEST ( x == z ) ;

  boost::value_initialized<T> const x_c ;
  BOOST_TEST ( y == x_c ) ;
  BOOST_TEST ( y == boost::get(x_c) ) ;
  T& x_c_ref = const_cast<T&>( boost::get(x_c) ) ;
  x_c_ref = z ;
  BOOST_TEST ( x_c == z ) ;

  boost::value_initialized<T> const copy1 = x;
  BOOST_TEST ( boost::get(copy1) == boost::get(x) ) ;

  boost::value_initialized<T> copy2;
  copy2 = x;
  BOOST_TEST ( boost::get(copy2) == boost::get(x) ) ;
  
  boost::shared_ptr<boost::value_initialized<T> > ptr( new boost::value_initialized<T> );
  BOOST_TEST ( y == *ptr ) ;

#if !BOOST_WORKAROUND(BOOST_MSVC, < 1300)
  boost::value_initialized<T const> cx ;
  BOOST_TEST ( y == cx ) ;
  BOOST_TEST ( y == boost::get(cx) ) ;

  boost::value_initialized<T const> const cx_c ;
  BOOST_TEST ( y == cx_c ) ;
  BOOST_TEST ( y == boost::get(cx_c) ) ;
#endif

  return boost::detail::test_errors() == errors_before_test ;
}

int main(int, char **)
{
  BOOST_TEST ( test( 0,1234 ) ) ;
  BOOST_TEST ( test( 0.0,12.34 ) ) ;
  BOOST_TEST ( test( POD(0,0,0.0), POD('a',1234,56.78f) ) ) ;
  BOOST_TEST ( test( NonPOD( std::string() ), NonPOD( std::string("something") ) ) ) ;

  NonPOD NonPOD_object( std::string("NonPOD_object") );
  BOOST_TEST ( test<NonPOD *>( 0, &NonPOD_object ) ) ;

  AggregatePODStruct zeroInitializedAggregatePODStruct = { 0.0f, '\0', 0 };
  AggregatePODStruct nonZeroInitializedAggregatePODStruct = { 1.25f, 'a', -1 };
  BOOST_TEST ( test(zeroInitializedAggregatePODStruct, nonZeroInitializedAggregatePODStruct) );

  StringAndInt stringAndInt0;
  StringAndInt stringAndInt1;
  stringAndInt0.i = 0;
  stringAndInt1.i = 1;
  stringAndInt1.s = std::string("1");
  BOOST_TEST ( test(stringAndInt0, stringAndInt1) );

  StructWithDestructor structWithDestructor0;
  StructWithDestructor structWithDestructor1;
  structWithDestructor0.i = 0;
  structWithDestructor1.i = 1;
  BOOST_TEST ( test(structWithDestructor0, structWithDestructor1) );

  StructWithVirtualFunction structWithVirtualFunction0;
  StructWithVirtualFunction structWithVirtualFunction1;
  structWithVirtualFunction0.i = 0;
  structWithVirtualFunction1.i = 1;
  BOOST_TEST ( test(structWithVirtualFunction0, structWithVirtualFunction1) );

  DerivedFromAggregatePODStruct derivedFromAggregatePODStruct0;
  DerivedFromAggregatePODStruct derivedFromAggregatePODStruct1;
  static_cast<AggregatePODStruct &>(derivedFromAggregatePODStruct0) = zeroInitializedAggregatePODStruct;
  static_cast<AggregatePODStruct &>(derivedFromAggregatePODStruct1) = nonZeroInitializedAggregatePODStruct;
  BOOST_TEST ( test(derivedFromAggregatePODStruct0, derivedFromAggregatePODStruct1) );

  AggregatePODStructWrapper aggregatePODStructWrapper0;
  AggregatePODStructWrapper aggregatePODStructWrapper1;
  aggregatePODStructWrapper0.dataMember = zeroInitializedAggregatePODStruct;
  aggregatePODStructWrapper1.dataMember = nonZeroInitializedAggregatePODStruct;
  BOOST_TEST ( test(aggregatePODStructWrapper0, aggregatePODStructWrapper1) );

  ArrayOfBytes zeroInitializedArrayOfBytes = { 0 };
  boost::value_initialized<ArrayOfBytes> valueInitializedArrayOfBytes;
  BOOST_TEST (std::memcmp(get(valueInitializedArrayOfBytes), zeroInitializedArrayOfBytes, sizeof(ArrayOfBytes)) == 0);

  boost::value_initialized<ArrayOfBytes> valueInitializedArrayOfBytes2;
  valueInitializedArrayOfBytes2 = valueInitializedArrayOfBytes;
  BOOST_TEST (std::memcmp(get(valueInitializedArrayOfBytes), get(valueInitializedArrayOfBytes2), sizeof(ArrayOfBytes)) == 0);

  boost::value_initialized<CopyFunctionCallTester> copyFunctionCallTester1;
  BOOST_TEST ( ! get(copyFunctionCallTester1).is_copy_constructed);
  BOOST_TEST ( ! get(copyFunctionCallTester1).is_assignment_called);

  boost::value_initialized<CopyFunctionCallTester> copyFunctionCallTester2 = boost::value_initialized<CopyFunctionCallTester>(copyFunctionCallTester1);
  BOOST_TEST ( get(copyFunctionCallTester2).is_copy_constructed);
  BOOST_TEST ( ! get(copyFunctionCallTester2).is_assignment_called);

  boost::value_initialized<CopyFunctionCallTester> copyFunctionCallTester3;
  copyFunctionCallTester3 = boost::value_initialized<CopyFunctionCallTester>(copyFunctionCallTester1);
  BOOST_TEST ( ! get(copyFunctionCallTester3).is_copy_constructed);
  BOOST_TEST ( get(copyFunctionCallTester3).is_assignment_called);

  boost::value_initialized<SwapFunctionCallTester> swapFunctionCallTester1;
  boost::value_initialized<SwapFunctionCallTester> swapFunctionCallTester2;
  get(swapFunctionCallTester1).data = 1;
  get(swapFunctionCallTester2).data = 2;
  boost::swap(swapFunctionCallTester1, swapFunctionCallTester2);
  BOOST_TEST( get(swapFunctionCallTester1).data == 2 );
  BOOST_TEST( get(swapFunctionCallTester2).data == 1 );
  BOOST_TEST( get(swapFunctionCallTester1).is_custom_swap_called );
  BOOST_TEST( get(swapFunctionCallTester2).is_custom_swap_called );

  return boost::report_errors();
}


