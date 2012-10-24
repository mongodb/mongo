// Copyright 2009 Google Inc. All Rights Reserved.
//
// Various Google-specific casting templates.
//
// This code is compiled directly on many platforms, including client
// platforms like Windows, Mac, and embedded systems.  Before making
// any changes here, make sure that you're not breaking any platforms.
//

#ifndef BASE_CASTS_H_
#define BASE_CASTS_H_

#include <assert.h>         // for use with down_cast<>
#include <string.h>         // for memcpy
#include <limits.h>         // for enumeration casts and tests
#include <typeinfo>         // for enumeration casts and tests

#include "base/macros.h"


// Use implicit_cast as a safe version of static_cast or const_cast
// for upcasting in the type hierarchy (i.e. casting a pointer to Foo
// to a pointer to SuperclassOfFoo or casting a pointer to Foo to
// a const pointer to Foo).
// When you use implicit_cast, the compiler checks that the cast is safe.
// Such explicit implicit_casts are necessary in surprisingly many
// situations where C++ demands an exact type match instead of an
// argument type convertable to a target type.
//
// The From type can be inferred, so the preferred syntax for using
// implicit_cast is the same as for static_cast etc.:
//
//   implicit_cast<ToType>(expr)
//
// implicit_cast would have been part of the C++ standard library,
// but the proposal was submitted too late.  It will probably make
// its way into the language in the future.
template<typename To, typename From>
inline To implicit_cast(From const &f) {
  return f;
}


// When you upcast (that is, cast a pointer from type Foo to type
// SuperclassOfFoo), it's fine to use implicit_cast<>, since upcasts
// always succeed.  When you downcast (that is, cast a pointer from
// type Foo to type SubclassOfFoo), static_cast<> isn't safe, because
// how do you know the pointer is really of type SubclassOfFoo?  It
// could be a bare Foo, or of type DifferentSubclassOfFoo.  Thus,
// when you downcast, you should use this macro.  In debug mode, we
// use dynamic_cast<> to double-check the downcast is legal (we die
// if it's not).  In normal mode, we do the efficient static_cast<>
// instead.  Thus, it's important to test in debug mode to make sure
// the cast is legal!
//    This is the only place in the code we should use dynamic_cast<>.
// In particular, you SHOULDN'T be using dynamic_cast<> in order to
// do RTTI (eg code like this:
//    if (dynamic_cast<Subclass1>(foo)) HandleASubclass1Object(foo);
//    if (dynamic_cast<Subclass2>(foo)) HandleASubclass2Object(foo);
// You should design the code some other way not to need this.

template<typename To, typename From>     // use like this: down_cast<T*>(foo);
inline To down_cast(From* f) {                   // so we only accept pointers
  // Ensures that To is a sub-type of From *.  This test is here only
  // for compile-time type checking, and has no overhead in an
  // optimized build at run-time, as it will be optimized away
  // completely.

  // TODO(user): This should use COMPILE_ASSERT.
  if (false) {
    implicit_cast<From*, To>(0);
  }

  // uses RTTI in dbg and fastbuild. asserts are disabled in opt builds.
  assert(f == NULL || dynamic_cast<To>(f) != NULL);
  return static_cast<To>(f);
}

// Overload of down_cast for references. Use like this: down_cast<T&>(foo).
// The code is slightly convoluted because we're still using the pointer
// form of dynamic cast. (The reference form throws an exception if it
// fails.)
//
// There's no need for a special const overload either for the pointer
// or the reference form. If you call down_cast with a const T&, the
// compiler will just bind From to const T.
template<typename To, typename From>
inline To down_cast(From& f) {
  COMPILE_ASSERT(base::is_reference<To>::value, target_type_not_a_reference);
  typedef typename base::remove_reference<To>::type* ToAsPointer;
  if (false) {
    // Compile-time check that To inherits from From. See above for details.
    implicit_cast<From*, ToAsPointer>(0);
  }

  assert(dynamic_cast<ToAsPointer>(&f) != NULL);  // RTTI: debug mode only
  return static_cast<To>(f);
}

// bit_cast<Dest,Source> is a template function that implements the
// equivalent of "*reinterpret_cast<Dest*>(&source)".  We need this in
// very low-level functions like the protobuf library and fast math
// support.
//
//   float f = 3.14159265358979;
//   int i = bit_cast<int32>(f);
//   // i = 0x40490fdb
//
// The classical address-casting method is:
//
//   // WRONG
//   float f = 3.14159265358979;            // WRONG
//   int i = * reinterpret_cast<int*>(&f);  // WRONG
//
// The address-casting method actually produces undefined behavior
// according to ISO C++ specification section 3.10 -15 -.  Roughly, this
// section says: if an object in memory has one type, and a program
// accesses it with a different type, then the result is undefined
// behavior for most values of "different type".
//
// This is true for any cast syntax, either *(int*)&f or
// *reinterpret_cast<int*>(&f).  And it is particularly true for
// conversions betweeen integral lvalues and floating-point lvalues.
//
// The purpose of 3.10 -15- is to allow optimizing compilers to assume
// that expressions with different types refer to different memory.  gcc
// 4.0.1 has an optimizer that takes advantage of this.  So a
// non-conforming program quietly produces wildly incorrect output.
//
// The problem is not the use of reinterpret_cast.  The problem is type
// punning: holding an object in memory of one type and reading its bits
// back using a different type.
//
// The C++ standard is more subtle and complex than this, but that
// is the basic idea.
//
// Anyways ...
//
// bit_cast<> calls memcpy() which is blessed by the standard,
// especially by the example in section 3.9 .  Also, of course,
// bit_cast<> wraps up the nasty logic in one place.
//
// Fortunately memcpy() is very fast.  In optimized mode, with a
// constant size, gcc 2.95.3, gcc 4.0.1, and msvc 7.1 produce inline
// code with the minimal amount of data movement.  On a 32-bit system,
// memcpy(d,s,4) compiles to one load and one store, and memcpy(d,s,8)
// compiles to two loads and two stores.
//
// I tested this code with gcc 2.95.3, gcc 4.0.1, icc 8.1, and msvc 7.1.
//
// WARNING: if Dest or Source is a non-POD type, the result of the memcpy
// is likely to surprise you.
//
// Props to Bill Gibbons for the compile time assertion technique and
// Art Komninos and Igor Tandetnik for the msvc experiments.
//
// -- mec 2005-10-17

template <class Dest, class Source>
inline Dest bit_cast(const Source& source) {
  // Compile time assertion: sizeof(Dest) == sizeof(Source)
  // A compile error here means your Dest and Source have different sizes.
  typedef char VerifySizesAreEqual [sizeof(Dest) == sizeof(Source) ? 1 : -1];

  Dest dest;
  memcpy(&dest, &source, sizeof(dest));
  return dest;
}


// **** Enumeration Casts and Tests
//
// C++ requires that the value of an integer that is converted to an
// enumeration be within the value bounds of the enumeration.  Modern
// compilers can and do take advantage of this requirement to optimize
// programs.  So, using a raw static_cast with enums can be bad.  See
//
// The following templates and macros enable casting from an int to an enum
// with checking against the appropriate bounds.  First, when defining an
// enumeration, identify the limits of the values of its enumerators.
//
//   enum A { A_min = -18, A_max = 33 };
//   MAKE_ENUM_LIMITS(A, A_min, A_max)
//
// Convert an enum to an int in one of two ways.  The prefered way is a
// tight conversion, which ensures that A_min <= value <= A_max.
//
//   A var = tight_enum_cast<A>(3);
//
// However, the C++ language defines the set of possible values for an
// enumeration to be essentially the range of a bitfield that can represent
// all the enumerators, i.e. those within the nearest containing power
// of two.  In the example above, the nearest positive power of two is 64,
// and so the upper bound is 63.  The nearest negative power of two is
// -32 and so the lower bound is -32 (two's complement), which is upgraded
// to match the upper bound, becoming -64.  The values within this range
// of -64 to 63 are valid, according to the C++ standard.  You can cast
// values within this range as follows.
//
//   A var = loose_enum_cast<A>(45);
//
// These casts will log a message if the value does not reside within the
// specified range, and will be fatal when in debug mode.
//
// For those times when an assert too strong, there are test functions.
//
//   bool var = tight_enum_test<A>(3);
//   bool var = loose_enum_test<A>(45);
//
// For code that needs to use the enumeration value if and only if
// it is good, there is a function that both tests and casts.
//
//   int i = ....;
//   A var;
//   if (tight_enum_test_cast<A>(i, &var))
//     .... // use valid var with value as indicated by i
//   else
//     .... // handle invalid enum cast
//
// The enum test/cast facility is currently limited to enumerations that
// fit within an int.  It is also limited to two's complement ints.

// ** Implementation Description
//
// The enum_limits template class captures the minimum and maximum
// enumerator.  All uses of this template are intended to be of
// specializations, so the generic has a field to identify itself as
// not specialized.  The test/cast templates assert specialization.

template <typename Enum>
class enum_limits {
 public:
  static const Enum min_enumerator = 0;
  static const Enum max_enumerator = 0;
  static const bool is_specialized = false;
};

// Now we define the macro to define the specialization for enum_limits.
// The specialization checks that the enumerators fit within an int.
// This checking relies on integral promotion.

#define MAKE_ENUM_LIMITS(ENUM_TYPE, ENUM_MIN, ENUM_MAX) \
template <> \
class enum_limits<ENUM_TYPE> { \
public: \
  static const ENUM_TYPE min_enumerator = ENUM_MIN; \
  static const ENUM_TYPE max_enumerator = ENUM_MAX; \
  static const bool is_specialized = true; \
  COMPILE_ASSERT(ENUM_MIN >= INT_MIN, enumerator_too_negative_for_int); \
  COMPILE_ASSERT(ENUM_MAX <= INT_MAX, enumerator_too_positive_for_int); \
};

// The loose enum test/cast is actually the more complicated one,
// because of the problem of finding the bounds.
//
// The unary upper bound, ub, on a positive number is its positive
// saturation, i.e. for a value v within pow(2,k-1) <= v < pow(2,k),
// the upper bound is pow(2,k)-1.
//
// The unary lower bound, lb, on a negative number is its negative
// saturation, i.e. for a value v within -pow(2,k) <= v < -pow(2,k-1),
// the lower bound is -pow(2,k).
//
// The actual bounds are (1) the binary upper bound over the maximum
// enumerator and the one's complement of a negative minimum enumerator
// and (2) the binary lower bound over the minimum enumerator and the
// one's complement of the positive maximum enumerator, except that if no
// enumerators are negative, the lower bound is zero.
//
// The algorithm relies heavily on the observation that
//
//   a,b>0 then ub(a,b) == ub(a) | ub(b) == ub(a|b)
//   a,b<0 then lb(a,b) == lb(a) & lb(b) == lb(a&b)
//
// Note that the compiler will boil most of this code away
// because of value propagation on the constant enumerator bounds.

template <typename Enum>
inline bool loose_enum_test(int e_val) {
  COMPILE_ASSERT(enum_limits<Enum>::is_specialized, missing_MAKE_ENUM_LIMITS);
  const Enum e_min = enum_limits<Enum>::min_enumerator;
  const Enum e_max = enum_limits<Enum>::max_enumerator;
  COMPILE_ASSERT(sizeof(e_val) == 4 || sizeof(e_val) == 8, unexpected_int_size);

  // Find the unary bounding negative number of e_min and e_max.

  // Find the unary bounding negative number of e_max.
  // This would be b_min = e_max < 0 ? e_max : ~e_max,
  // but we want to avoid branches to help the compiler.
  int e_max_sign = e_max >> (sizeof(e_val)*8 - 1);
  int b_min = ~e_max_sign ^ e_max;

  // Find the binary bounding negative of both e_min and e_max.
  b_min &= e_min;

  // However, if e_min is postive, the result will be positive.
  // Now clear all bits right of the most significant clear bit,
  // which is a negative saturation for negative numbers.
  // In the case of positive numbers, this is flush to zero.
  b_min &= b_min >> 1;
  b_min &= b_min >> 2;
  b_min &= b_min >> 4;
  b_min &= b_min >> 8;
  b_min &= b_min >> 16;
#if INT_MAX > 2147483647
  b_min &= b_min >> 32;
#endif

  // Find the unary bounding positive number of e_max.
  int b_max = e_max_sign ^ e_max;

  // Find the binary bounding postive number of that
  // and the unary bounding positive number of e_min.
  int e_min_sign = e_min >> (sizeof(e_val)*8 - 1);
  b_max |= e_min_sign ^ e_min;

  // Now set all bits right of the most significant set bit,
  // which is a postive saturation for positive numbers.
  b_max |= b_max >> 1;
  b_max |= b_max >> 2;
  b_max |= b_max >> 4;
  b_max |= b_max >> 8;
  b_max |= b_max >> 16;
#if INT_MAX > 2147483647
  b_max |= b_max >> 32;
#endif

  // Finally test the bounds.
  return b_min <= e_val && e_val <= b_max;
}

template <typename Enum>
inline bool tight_enum_test(int e_val) {
  COMPILE_ASSERT(enum_limits<Enum>::is_specialized, missing_MAKE_ENUM_LIMITS);
  const Enum e_min = enum_limits<Enum>::min_enumerator;
  const Enum e_max = enum_limits<Enum>::max_enumerator;
  return e_min <= e_val && e_val <= e_max;
}

template <typename Enum>
inline bool loose_enum_test_cast(int e_val, Enum* e_var) {
  if (loose_enum_test<Enum>(e_val)) {
     *e_var = static_cast<Enum>(e_val);
     return true;
  } else {
     return false;
  }
}

template <typename Enum>
inline bool tight_enum_test_cast(int e_val, Enum* e_var) {
  if (tight_enum_test<Enum>(e_val)) {
     *e_var = static_cast<Enum>(e_val);
     return true;
  } else {
     return false;
  }
}

// The plain casts require logging, and we get header recursion if
// it is done directly.  So, we do it indirectly.
// The following function is defined in logging.cc.

namespace logging {

void WarnEnumCastError(const char* name_of_type, int value_of_int);

}  // namespace logging

template <typename Enum>
inline Enum loose_enum_cast(int e_val) {
  if (!loose_enum_test<Enum>(e_val)) {
#if __GNUC__ && !__GXX_RTTI
    // Gcc and -fno-rtti; can't issue a warning with enum name.
    assert(false);
#else
    logging::WarnEnumCastError(typeid(Enum).name(), e_val);
#endif
  }
  return static_cast<Enum>(e_val);
}

template <typename Enum>
inline Enum tight_enum_cast(int e_val) {
  if (!tight_enum_test<Enum>(e_val)) {
#if __GNUC__ && !__GXX_RTTI
    // Gcc and -fno-rtti; can't issue a warning with enum name.
    assert(false);
#else
    logging::WarnEnumCastError(typeid(Enum).name(), e_val);
#endif
  }
  return static_cast<Enum>(e_val);
}

#endif  // BASE_CASTS_H_
