// Copyright 2008 Google Inc. All Rights Reserved.
//
// Various Google-specific macros.
//
// This code is compiled directly on many platforms, including client
// platforms like Windows, Mac, and embedded systems.  Before making
// any changes here, make sure that you're not breaking any platforms.
//

#ifndef BASE_MACROS_H_
#define BASE_MACROS_H_

#include "base/definer.h"   // For OS_WINDOWS

#include <stddef.h>         // For size_t

// We use our own  local  version of type traits while we're waiting
// for TR1 type traits to be standardized. Define some macros so that
// most google3 code doesn't have to work with type traits directly.
#include "type_traits.h"

// The swigged version of an abstract class must be concrete if any methods
// return objects of the abstract type. We keep it abstract in C++ and
// concrete for swig.
#ifndef SWIG
#define ABSTRACT = 0
#endif

// The COMPILE_ASSERT macro can be used to verify that a compile time
// expression is true. For example, you could use it to verify the
// size of a static array:
//
//   COMPILE_ASSERT(ARRAYSIZE(content_type_names) == CONTENT_NUM_TYPES,
//                  content_type_names_incorrect_size);
//
// or to make sure a struct is smaller than a certain size:
//
//   COMPILE_ASSERT(sizeof(foo) < 128, foo_too_large);
//
// The second argument to the macro is the name of the variable. If
// the expression is false, most compilers will issue a warning/error
// containing the name of the variable.

template <bool>
struct CompileAssert {
};

#define COMPILE_ASSERT(expr, msg) \
  typedef CompileAssert<(bool(expr))> msg[bool(expr) ? 1 : -1]

// Implementation details of COMPILE_ASSERT:
//
// - COMPILE_ASSERT works by defining an array type that has -1
//   elements (and thus is invalid) when the expression is false.
//
// - The simpler definition
//
//     #define COMPILE_ASSERT(expr, msg) typedef char msg[(expr) ? 1 : -1]
//
//   does not work, as gcc supports variable-length arrays whose sizes
//   are determined at run-time (this is gcc's extension and not part
//   of the C++ standard).  As a result, gcc fails to reject the
//   following code with the simple definition:
//
//     int foo;
//     COMPILE_ASSERT(foo, msg); // not supposed to compile as foo is
//                               // not a compile-time constant.
//
// - By using the type CompileAssert<(bool(expr))>, we ensures that
//   expr is a compile-time constant.  (Template arguments must be
//   determined at compile-time.)
//
// - The outter parentheses in CompileAssert<(bool(expr))> are necessary
//   to work around a bug in gcc 3.4.4 and 4.0.1.  If we had written
//
//     CompileAssert<bool(expr)>
//
//   instead, these compilers will refuse to compile
//
//     COMPILE_ASSERT(5 > 0, some_message);
//
//   (They seem to think the ">" in "5 > 0" marks the end of the
//   template argument list.)
//
// - The array size is (bool(expr) ? 1 : -1), instead of simply
//
//     ((expr) ? 1 : -1).
//
//   This is to avoid running into a bug in MS VC 7.1, which
//   causes ((0.0) ? 1 : -1) to incorrectly evaluate to 1.


// A macro to disallow the copy constructor and operator= functions
// This should be used in the private: declarations for a class
//
// For disallowing only assign or copy, write the code directly, but declare
// the intend in a comment, for example:
// void operator=(const TypeName&);  // DISALLOW_ASSIGN
// Note, that most uses of DISALLOW_ASSIGN and DISALLOW_COPY are broken
// semantically, one should either use disallow both or neither. Try to
// avoid these in new code.
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&);               \
  void operator=(const TypeName&)

// An older, politically incorrect name for the above.
// Prefer DISALLOW_COPY_AND_ASSIGN for new code.
#define DISALLOW_EVIL_CONSTRUCTORS(TypeName) DISALLOW_COPY_AND_ASSIGN(TypeName)

// A macro to disallow all the implicit constructors, namely the
// default constructor, copy constructor and operator= functions.
//
// This should be used in the private: declarations for a class
// that wants to prevent anyone from instantiating it. This is
// especially useful for classes containing only static methods.
#define DISALLOW_IMPLICIT_CONSTRUCTORS(TypeName) \
  TypeName();                                    \
  DISALLOW_COPY_AND_ASSIGN(TypeName)

// The arraysize(arr) macro returns the # of elements in an array arr.
// The expression is a compile-time constant, and therefore can be
// used in defining new arrays, for example.  If you use arraysize on
// a pointer by mistake, you will get a compile-time error.
//
// One caveat is that arraysize() doesn't accept any array of an
// anonymous type or a type defined inside a function.  In these rare
// cases, you have to use the unsafe ARRAYSIZE() macro below.  This is
// due to a limitation in C++'s template system.  The limitation might
// eventually be removed, but it hasn't happened yet.

// This template function declaration is used in defining arraysize.
// Note that the function doesn't need an implementation, as we only
// use its type.
template <typename T, size_t N>
char (&ArraySizeHelper(T (&array)[N]))[N];

// That gcc wants both of these prototypes seems mysterious. VC, for
// its part, can't decide which to use (another mystery). Matching of
// template overloads: the final frontier.
#ifndef COMPILER_MSVC
template <typename T, size_t N>
char (&ArraySizeHelper(const T (&array)[N]))[N];
#endif

#define arraysize(array) (sizeof(ArraySizeHelper(array)))

// ARRAYSIZE performs essentially the same calculation as arraysize,
// but can be used on anonymous types or types defined inside
// functions.  It's less safe than arraysize as it accepts some
// (although not all) pointers.  Therefore, you should use arraysize
// whenever possible.
//
// The expression ARRAYSIZE(a) is a compile-time constant of type
// size_t.
//
// ARRAYSIZE catches a few type errors.  If you see a compiler error
//
//   "warning: division by zero in ..."
//
// when using ARRAYSIZE, you are (wrongfully) giving it a pointer.
// You should only use ARRAYSIZE on statically allocated arrays.
//
// The following comments are on the implementation details, and can
// be ignored by the users.
//
// ARRAYSIZE(arr) works by inspecting sizeof(arr) (the # of bytes in
// the array) and sizeof(*(arr)) (the # of bytes in one array
// element).  If the former is divisible by the latter, perhaps arr is
// indeed an array, in which case the division result is the # of
// elements in the array.  Otherwise, arr cannot possibly be an array,
// and we generate a compiler error to prevent the code from
// compiling.
//
// Since the size of bool is implementation-defined, we need to cast
// !(sizeof(a) & sizeof(*(a))) to size_t in order to ensure the final
// result has type size_t.
//
// This macro is not perfect as it wrongfully accepts certain
// pointers, namely where the pointer size is divisible by the pointee
// size.  Since all our code has to go through a 32-bit compiler,
// where a pointer is 4 bytes, this means all pointers to a type whose
// size is 3 or greater than 4 will be (righteously) rejected.
//
// Kudos to Jorg Brown for this simple and elegant implementation.
//
// - wan 2005-11-16
//
// Starting with Visual C++ 2005, WinNT.h includes ARRAYSIZE.
#ifndef OS_WINDOWS
#if !defined(COMPILER_MSVC) || (defined(_MSC_VER) && _MSC_VER < 1400)
#define ARRAYSIZE(a) \
  ((sizeof(a) / sizeof(*(a))) / \
   static_cast<size_t>(!(sizeof(a) % sizeof(*(a)))))
#endif
#endif

// A macro to turn a symbol into a string
#define AS_STRING(x)   AS_STRING_INTERNAL(x)
#define AS_STRING_INTERNAL(x)   #x


// One of the type traits, is_pod, makes it possible to query whether
// a type is a POD type. It is impossible for type_traits.h to get
// this right without compiler support, so it fails conservatively. It
// knows that fundamental types and pointers are PODs, but it can't
// tell whether user classes are PODs. The DECLARE_POD macro is used
// to inform the type traits library that a user class is a POD.
//
// Implementation note: the typedef at the end is just to make it legal
// to put a semicolon after DECLARE_POD(foo).
//
// The only reason this matters is that a few parts of the google3
// code base either require their template arguments to be PODs
// (e.g. compact_vector) or are able to use a more efficient code path
// when their template arguments are PODs (e.g. sparse_hash_map). You
// should use DECLARE_POD if you have written a class that you intend
// to use with one of those components, and if you know that your
// class satisfies all of the conditions to be a POD type.
//
// So what's a POD?  The C++ standard (clause 9 paragraph 4) gives a
// full definition, but a good rule of thumb is that a struct is a POD
// ("plain old data") if it doesn't use any of the features that make
// C++ different from C.  A POD struct can't have constructors,
// destructors, assignment operators, base classes, private or
// protected members, or virtual functions, and all of its member
// variables must themselves be PODs.

#define DECLARE_POD(TypeName)                       \
namespace base {                                    \
template<> struct is_pod<TypeName> : true_type { }; \
}                                                   \
typedef int Dummy_Type_For_DECLARE_POD              \

// We once needed a different technique to assert that a nested class
// is a POD. This is no longer necessary, and DECLARE_NESTED_POD is
// just a synonym for DECLARE_POD. We continue to provide
// DECLARE_NESTED_POD only so we don't have to change client
// code. Regardless of whether you use DECLARE_POD or
// DECLARE_NESTED_POD: use it after the outer class. Using it within a
// class definition will give a compiler error.
#define DECLARE_NESTED_POD(TypeName) DECLARE_POD(TypeName)

// Declare that TemplateName<T> is a POD whenever T is
#define PROPAGATE_POD_FROM_TEMPLATE_ARGUMENT(TemplateName)             \
namespace base {                                                       \
template <typename T> struct is_pod<TemplateName<T> > : is_pod<T> { }; \
}                                                                      \
typedef int Dummy_Type_For_PROPAGATE_POD_FROM_TEMPLATE_ARGUMENT

// Macro that does nothing if TypeName is a POD, and gives a compiler
// error if TypeName is a non-POD.  You should put a descriptive
// comment right next to the macro call so that people can tell what
// the compiler error is about.
//
// Implementation note: this works by taking the size of a type that's
// complete when TypeName is a POD and incomplete otherwise.

template <bool IsPod> struct ERROR_TYPE_MUST_BE_POD;
template <> struct ERROR_TYPE_MUST_BE_POD<true> { };
#define ENFORCE_POD(TypeName)                                             \
  enum { dummy_##TypeName                                                 \
           = sizeof(ERROR_TYPE_MUST_BE_POD<                               \
                      base::is_pod<TypeName>::value>) }

#endif  // BASE_MACROS_H_
