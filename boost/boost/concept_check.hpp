//
// (C) Copyright Jeremy Siek 2000.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// Revision History:
//   05 May   2001: Workarounds for HP aCC from Thomas Matelich. (Jeremy Siek)
//   02 April 2001: Removed limits header altogether. (Jeremy Siek)
//   01 April 2001: Modified to use new <boost/limits.hpp> header. (JMaddock)
//

// See http://www.boost.org/libs/concept_check for documentation.

#ifndef BOOST_CONCEPT_CHECKS_HPP
#define BOOST_CONCEPT_CHECKS_HPP

#include <boost/config.hpp>
#include <boost/iterator.hpp>
#include <boost/type_traits/conversion_traits.hpp>
#include <utility>
#include <boost/type_traits/conversion_traits.hpp>
#include <boost/static_assert.hpp>
#include <boost/mpl/identity.hpp>


#if defined(BOOST_MSVC) && BOOST_MSVC <= 1300 || defined(__BORLANDC__)
#define BOOST_FPTR
#else
#define BOOST_FPTR &
#endif

namespace boost {

/*
  "inline" is used for ignore_unused_variable_warning()
   and function_requires() to make sure there is no
   overhead with g++.
 */

template <class T> inline void ignore_unused_variable_warning(const T&) { }

// the unused, defaulted parameter is a workaround for MSVC and Compaq C++
template <class Concept>
inline void function_requires(mpl::identity<Concept>* = 0)
{
#if !defined(NDEBUG)
  void (Concept::*x)() = BOOST_FPTR Concept::constraints;
  ignore_unused_variable_warning(x);
#endif
}

#define BOOST_CLASS_REQUIRE(type_var, ns, concept) \
  typedef void (ns::concept <type_var>::* func##type_var##concept)(); \
  template <func##type_var##concept Tp1_> \
  struct concept_checking_##type_var##concept { }; \
  typedef concept_checking_##type_var##concept< \
    BOOST_FPTR ns::concept<type_var>::constraints> \
    concept_checking_typedef_##type_var##concept

#define BOOST_CLASS_REQUIRE2(type_var1, type_var2, ns, concept) \
  typedef void (ns::concept <type_var1,type_var2>::* \
     func##type_var1##type_var2##concept)(); \
  template <func##type_var1##type_var2##concept Tp1_> \
  struct concept_checking_##type_var1##type_var2##concept { }; \
  typedef concept_checking_##type_var1##type_var2##concept< \
    BOOST_FPTR ns::concept<type_var1,type_var2>::constraints> \
    concept_checking_typedef_##type_var1##type_var2##concept

#define BOOST_CLASS_REQUIRE3(tv1, tv2, tv3, ns, concept) \
  typedef void (ns::concept <tv1,tv2,tv3>::* \
     func##tv1##tv2##tv3##concept)(); \
  template <func##tv1##tv2##tv3##concept Tp1_> \
  struct concept_checking_##tv1##tv2##tv3##concept { }; \
  typedef concept_checking_##tv1##tv2##tv3##concept< \
    BOOST_FPTR ns::concept<tv1,tv2,tv3>::constraints> \
    concept_checking_typedef_##tv1##tv2##tv3##concept

#define BOOST_CLASS_REQUIRE4(tv1, tv2, tv3, tv4, ns, concept) \
  typedef void (ns::concept <tv1,tv2,tv3,tv4>::* \
     func##tv1##tv2##tv3##tv4##concept)(); \
  template <func##tv1##tv2##tv3##tv4##concept Tp1_> \
  struct concept_checking_##tv1##tv2##tv3##tv4##concept { }; \
  typedef concept_checking_##tv1##tv2##tv3##tv4##concept< \
    BOOST_FPTR ns::concept<tv1,tv2,tv3,tv4>::constraints> \
    concept_checking_typedef_##tv1##tv2##tv3##tv4##concept

// NOTE: The BOOST_CLASS_REQUIRES (with an 'S' at the end) is deprecated.

// The BOOST_CLASS_REQUIRES macros use function pointers as
// template parameters, which VC++ does not support.

#if defined(BOOST_NO_FUNCTION_PTR_TEMPLATE_PARAMETERS)

#define BOOST_CLASS_REQUIRES(type_var, concept)
#define BOOST_CLASS_REQUIRES2(type_var1, type_var2, concept)
#define BOOST_CLASS_REQUIRES3(type_var1, type_var2, type_var3, concept)
#define BOOST_CLASS_REQUIRES4(type_var1, type_var2, type_var3, type_var4, concept)

#else

#define BOOST_CLASS_REQUIRES(type_var, concept) \
  typedef void (concept <type_var>::* func##type_var##concept)(); \
  template <func##type_var##concept Tp1_> \
  struct concept_checking_##type_var##concept { }; \
  typedef concept_checking_##type_var##concept< \
    BOOST_FPTR concept <type_var>::constraints> \
    concept_checking_typedef_##type_var##concept

#define BOOST_CLASS_REQUIRES2(type_var1, type_var2, concept) \
  typedef void (concept <type_var1,type_var2>::* func##type_var1##type_var2##concept)(); \
  template <func##type_var1##type_var2##concept Tp1_> \
  struct concept_checking_##type_var1##type_var2##concept { }; \
  typedef concept_checking_##type_var1##type_var2##concept< \
    BOOST_FPTR concept <type_var1,type_var2>::constraints> \
    concept_checking_typedef_##type_var1##type_var2##concept

#define BOOST_CLASS_REQUIRES3(type_var1, type_var2, type_var3, concept) \
  typedef void (concept <type_var1,type_var2,type_var3>::* func##type_var1##type_var2##type_var3##concept)(); \
  template <func##type_var1##type_var2##type_var3##concept Tp1_> \
  struct concept_checking_##type_var1##type_var2##type_var3##concept { }; \
  typedef concept_checking_##type_var1##type_var2##type_var3##concept< \
    BOOST_FPTR concept <type_var1,type_var2,type_var3>::constraints>  \
  concept_checking_typedef_##type_var1##type_var2##type_var3##concept

#define BOOST_CLASS_REQUIRES4(type_var1, type_var2, type_var3, type_var4, concept) \
  typedef void (concept <type_var1,type_var2,type_var3,type_var4>::* func##type_var1##type_var2##type_var3##type_var4##concept)(); \
  template <func##type_var1##type_var2##type_var3##type_var4##concept Tp1_> \
  struct concept_checking_##type_var1##type_var2##type_var3##type_var4##concept { }; \
  typedef concept_checking_##type_var1##type_var2##type_var3##type_var4##concept< \
    BOOST_FPTR concept <type_var1,type_var2,type_var3,type_var4>::constraints>  \
    concept_checking_typedef_##type_var1##type_var2##type_var3##type_var4##concept


#endif

#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
template <class T, class U>
struct require_same { };

template <class T>
struct require_same<T,T> { typedef T type; };
#else
// This version does not perform checking, but will not do any harm.
template <class T, class U>
struct require_same { typedef T type; };
#endif

  template <class T>
  struct IntegerConcept {
    void constraints() { 
#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
      x.error_type_must_be_an_integer_type();
#endif      
    }
    T x;
  };
#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
  template <> struct IntegerConcept<short> { void constraints() {} };
  template <> struct IntegerConcept<unsigned short> { void constraints() {} };
  template <> struct IntegerConcept<int> { void constraints() {} };
  template <> struct IntegerConcept<unsigned int> { void constraints() {} };
  template <> struct IntegerConcept<long> { void constraints() {} };
  template <> struct IntegerConcept<unsigned long> { void constraints() {} };
  // etc.
#endif      

  template <class T>
  struct SignedIntegerConcept {
    void constraints() { 
#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
      x.error_type_must_be_a_signed_integer_type();
#endif      
    }
    T x;
  };
#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
  template <> struct SignedIntegerConcept<short> { void constraints() {} };
  template <> struct SignedIntegerConcept<int> { void constraints() {} };
  template <> struct SignedIntegerConcept<long> { void constraints() {} };
# if defined(BOOST_HAS_LONG_LONG)
  template <> struct SignedIntegerConcept< ::boost::long_long_type> { void constraints() {} };
# endif
  // etc.
#endif      

  template <class T>
  struct UnsignedIntegerConcept {
    void constraints() { 
#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
      x.error_type_must_be_an_unsigned_integer_type();
#endif      
    }
    T x;
  };
#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
  template <> struct UnsignedIntegerConcept<unsigned short>
    { void constraints() {} };
  template <> struct UnsignedIntegerConcept<unsigned int>
    { void constraints() {} };
  template <> struct UnsignedIntegerConcept<unsigned long>
    { void constraints() {} };
  // etc.
#endif      

  //===========================================================================
  // Basic Concepts

  template <class TT>
  struct DefaultConstructibleConcept
  {
    void constraints() {
      TT a;               // require default constructor
      ignore_unused_variable_warning(a);
    }
  };

  template <class TT>
  struct AssignableConcept
  {
    void constraints() {
#if !defined(_ITERATOR_) // back_insert_iterator broken for VC++ STL
      a = a;              // require assignment operator
#endif
      const_constraints(a);
    }
    void const_constraints(const TT& b) {
#if !defined(_ITERATOR_) // back_insert_iterator broken for VC++ STL
      a = b;              // const required for argument to assignment
#endif
    }
    TT a;
  };

  template <class TT>
  struct CopyConstructibleConcept
  {
    void constraints() {
      TT a(b);            // require copy constructor
      TT* ptr = &a;       // require address of operator
      const_constraints(a);
      ignore_unused_variable_warning(ptr);
    }
    void const_constraints(const TT& a) {
      TT c(a);            // require const copy constructor
      const TT* ptr = &a; // require const address of operator
      ignore_unused_variable_warning(c);
      ignore_unused_variable_warning(ptr);
    }
    TT b;
  };

  // The SGI STL version of Assignable requires copy constructor and operator=
  template <class TT>
  struct SGIAssignableConcept
  {
    void constraints() {
      TT b(a);
#if !defined(_ITERATOR_) // back_insert_iterator broken for VC++ STL
      a = a;              // require assignment operator
#endif
      const_constraints(a);
      ignore_unused_variable_warning(b);
    }
    void const_constraints(const TT& b) {
      TT c(b);
#if !defined(_ITERATOR_) // back_insert_iterator broken for VC++ STL
      a = b;              // const required for argument to assignment
#endif
      ignore_unused_variable_warning(c);
    }
    TT a;
  };

  template <class X, class Y>
  struct ConvertibleConcept
  {
    void constraints() {
      Y y = x;
      ignore_unused_variable_warning(y);
    }
    X x;
  };

  // The C++ standard requirements for many concepts talk about return
  // types that must be "convertible to bool".  The problem with this
  // requirement is that it leaves the door open for evil proxies that
  // define things like operator|| with strange return types.  Two
  // possible solutions are:
  // 1) require the return type to be exactly bool
  // 2) stay with convertible to bool, and also
  //    specify stuff about all the logical operators.
  // For now we just test for convertible to bool.
  template <class TT>
  void require_boolean_expr(const TT& t) {
    bool x = t;
    ignore_unused_variable_warning(x);
  }

  template <class TT>
  struct EqualityComparableConcept
  {
    void constraints() {
      require_boolean_expr(a == b);
      require_boolean_expr(a != b);
    }
    TT a, b;
  };

  template <class TT>
  struct LessThanComparableConcept
  {
    void constraints() {
      require_boolean_expr(a < b);
    }
    TT a, b;
  };

  // This is equivalent to SGI STL's LessThanComparable.
  template <class TT>
  struct ComparableConcept
  {
    void constraints() {
      require_boolean_expr(a < b);
      require_boolean_expr(a > b);
      require_boolean_expr(a <= b);
      require_boolean_expr(a >= b);
    }
    TT a, b;
  };

#define BOOST_DEFINE_BINARY_PREDICATE_OP_CONSTRAINT(OP,NAME) \
  template <class First, class Second> \
  struct NAME { \
    void constraints() { (void)constraints_(); } \
    bool constraints_() {  \
      return  a OP b; \
    } \
    First a; \
    Second b; \
  }

#define BOOST_DEFINE_BINARY_OPERATOR_CONSTRAINT(OP,NAME) \
  template <class Ret, class First, class Second> \
  struct NAME { \
    void constraints() { (void)constraints_(); } \
    Ret constraints_() {  \
      return a OP b; \
    } \
    First a; \
    Second b; \
  }

  BOOST_DEFINE_BINARY_PREDICATE_OP_CONSTRAINT(==, EqualOpConcept);
  BOOST_DEFINE_BINARY_PREDICATE_OP_CONSTRAINT(!=, NotEqualOpConcept);
  BOOST_DEFINE_BINARY_PREDICATE_OP_CONSTRAINT(<, LessThanOpConcept);
  BOOST_DEFINE_BINARY_PREDICATE_OP_CONSTRAINT(<=, LessEqualOpConcept);
  BOOST_DEFINE_BINARY_PREDICATE_OP_CONSTRAINT(>, GreaterThanOpConcept);
  BOOST_DEFINE_BINARY_PREDICATE_OP_CONSTRAINT(>=, GreaterEqualOpConcept);

  BOOST_DEFINE_BINARY_OPERATOR_CONSTRAINT(+, PlusOpConcept);
  BOOST_DEFINE_BINARY_OPERATOR_CONSTRAINT(*, TimesOpConcept);
  BOOST_DEFINE_BINARY_OPERATOR_CONSTRAINT(/, DivideOpConcept);
  BOOST_DEFINE_BINARY_OPERATOR_CONSTRAINT(-, SubtractOpConcept);
  BOOST_DEFINE_BINARY_OPERATOR_CONSTRAINT(%, ModOpConcept);

  //===========================================================================
  // Function Object Concepts

  template <class Func, class Return>
  struct GeneratorConcept
  {
    void constraints() {
      const Return& r = f();   // require operator() member function
      ignore_unused_variable_warning(r);
    }
    Func f;
  };


#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
  template <class Func>
  struct GeneratorConcept<Func,void>
  {
    void constraints() {
      f();              // require operator() member function
    }
    Func f;
  };
#endif

  template <class Func, class Return, class Arg>
  struct UnaryFunctionConcept
  {
    // required in case any of our template args are const-qualified:
    UnaryFunctionConcept();
    
    void constraints() {
      r = f(arg); // require operator()
    }
    Func f;
    Arg arg;
    Return r;
  };

#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
  template <class Func, class Arg>
  struct UnaryFunctionConcept<Func, void, Arg> {
    void constraints() { 
      f(arg);                 // require operator()
    }
    Func f;
    Arg arg;
  };
#endif

  template <class Func, class Return, class First, class Second>
  struct BinaryFunctionConcept
  {
    void constraints() { 
      r = f(first, second); // require operator()
    }
    Func f;
    First first;
    Second second;
    Return r;
  };

#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
  template <class Func, class First, class Second>
  struct BinaryFunctionConcept<Func, void, First, Second>
  {
    void constraints() {
      f(first, second); // require operator()
    }
    Func f;
    First first;
    Second second;
  };
#endif

  template <class Func, class Arg>
  struct UnaryPredicateConcept
  {
    void constraints() {
      require_boolean_expr(f(arg)); // require operator() returning bool
    }
    Func f;
    Arg arg;
  };

  template <class Func, class First, class Second>
  struct BinaryPredicateConcept
  {
    void constraints() {
      require_boolean_expr(f(a, b)); // require operator() returning bool
    }
    Func f;
    First a;
    Second b;
  };

  // use this when functor is used inside a container class like std::set
  template <class Func, class First, class Second>
  struct Const_BinaryPredicateConcept {
    void constraints() { 
      const_constraints(f);
    }
    void const_constraints(const Func& fun) {
      function_requires<BinaryPredicateConcept<Func, First, Second> >();
      // operator() must be a const member function
      require_boolean_expr(fun(a, b));
    }
    Func f;
    First a;
    Second b;
  };

  template <class Func, class Return>
  struct AdaptableGeneratorConcept
  {
    void constraints() {
      typedef typename Func::result_type result_type;
      BOOST_STATIC_ASSERT((is_convertible<result_type, Return>::value));
      function_requires< GeneratorConcept<Func, result_type> >();
    }
  };

  template <class Func, class Return, class Arg>
  struct AdaptableUnaryFunctionConcept
  {
    void constraints() {
      typedef typename Func::argument_type argument_type;
      typedef typename Func::result_type result_type;
      BOOST_STATIC_ASSERT((is_convertible<result_type, Return>::value));
      BOOST_STATIC_ASSERT((is_convertible<Arg, argument_type>::value));
      function_requires< UnaryFunctionConcept<Func, result_type, argument_type> >();
    }
  };

  template <class Func, class Return, class First, class Second>
  struct AdaptableBinaryFunctionConcept
  {
    void constraints() {
      typedef typename Func::first_argument_type first_argument_type;
      typedef typename Func::second_argument_type second_argument_type;
      typedef typename Func::result_type result_type;
      BOOST_STATIC_ASSERT((is_convertible<result_type, Return>::value));
      BOOST_STATIC_ASSERT((is_convertible<First, first_argument_type>::value));
      BOOST_STATIC_ASSERT((is_convertible<Second, second_argument_type>::value));
      function_requires< BinaryFunctionConcept<Func, result_type, 
        first_argument_type, second_argument_type> >();
    }
  };

  template <class Func, class Arg>
  struct AdaptablePredicateConcept
  {
    void constraints() {
      function_requires< UnaryPredicateConcept<Func, Arg> >();
      function_requires< AdaptableUnaryFunctionConcept<Func, bool, Arg> >();
    }
  };

  template <class Func, class First, class Second>
  struct AdaptableBinaryPredicateConcept
  {
    void constraints() {
      function_requires< BinaryPredicateConcept<Func, First, Second> >();
      function_requires< AdaptableBinaryFunctionConcept<Func, bool, First, Second> >();
    }
  };

  //===========================================================================
  // Iterator Concepts

  template <class TT>
  struct InputIteratorConcept
  {
    void constraints() {
      function_requires< AssignableConcept<TT> >();
      function_requires< EqualityComparableConcept<TT> >();
      TT j(i);
      (void)*i;           // require dereference operator
#ifndef BOOST_NO_STD_ITERATOR_TRAITS
      // require iterator_traits typedef's
      typedef typename std::iterator_traits<TT>::difference_type D;
      // Hmm, the following is a bit fragile
      //function_requires< SignedIntegerConcept<D> >();
      typedef typename std::iterator_traits<TT>::reference R;
      typedef typename std::iterator_traits<TT>::pointer P;
      typedef typename std::iterator_traits<TT>::iterator_category C;
      function_requires< ConvertibleConcept<C, std::input_iterator_tag> >();
#endif
      ++j;                // require preincrement operator
      i++;                // require postincrement operator
    }
    TT i;
  };

  template <class TT, class ValueT>
  struct OutputIteratorConcept
  {
    void constraints() {
      function_requires< AssignableConcept<TT> >();
      ++i;                // require preincrement operator
      i++;                // require postincrement operator
      *i++ = t;           // require postincrement and assignment
    }
    TT i, j;
    ValueT t;
  };

  template <class TT>
  struct ForwardIteratorConcept
  {
    void constraints() {
      function_requires< InputIteratorConcept<TT> >();
#ifndef BOOST_NO_STD_ITERATOR_TRAITS
      typedef typename std::iterator_traits<TT>::iterator_category C;
      function_requires< ConvertibleConcept<C, std::forward_iterator_tag> >();
      typedef typename std::iterator_traits<TT>::reference reference;
      reference r = *i;
      ignore_unused_variable_warning(r);
#endif
    }
    TT i;
  };

  template <class TT>
  struct Mutable_ForwardIteratorConcept
  {
    void constraints() {
      function_requires< ForwardIteratorConcept<TT> >();
      *i++ = *i;         // require postincrement and assignment
    }
    TT i;
  };

  template <class TT>
  struct BidirectionalIteratorConcept
  {
    void constraints() {
      function_requires< ForwardIteratorConcept<TT> >();
#ifndef BOOST_NO_STD_ITERATOR_TRAITS
      typedef typename std::iterator_traits<TT>::iterator_category C;
      function_requires< ConvertibleConcept<C, 
        std::bidirectional_iterator_tag> >();
#endif
      --i;                // require predecrement operator
      i--;                // require postdecrement operator
    }
    TT i;
  };

  template <class TT>
  struct Mutable_BidirectionalIteratorConcept
  {
    void constraints() {
      function_requires< BidirectionalIteratorConcept<TT> >();
      function_requires< Mutable_ForwardIteratorConcept<TT> >();
      *i-- = *i;                  // require postdecrement and assignment
    }
    TT i;
  };


  template <class TT>
  struct RandomAccessIteratorConcept
  {
    void constraints() {
      function_requires< BidirectionalIteratorConcept<TT> >();
      function_requires< ComparableConcept<TT> >();
#ifndef BOOST_NO_STD_ITERATOR_TRAITS
      typedef typename std::iterator_traits<TT>::iterator_category C;
      function_requires< ConvertibleConcept< C,
        std::random_access_iterator_tag> >();
      typedef typename std::iterator_traits<TT>::reference R;
#endif

      i += n;             // require assignment addition operator
      i = i + n; i = n + i; // require addition with difference type
      i -= n;             // require assignment subtraction operator
      i = i - n;                  // require subtraction with difference type
      n = i - j;                  // require difference operator
      (void)i[n];                 // require element access operator
    }
    TT a, b;
    TT i, j;
#ifndef BOOST_NO_STD_ITERATOR_TRAITS
    typename std::iterator_traits<TT>::difference_type n;
#else
    std::ptrdiff_t n;
#endif
  };

  template <class TT>
  struct Mutable_RandomAccessIteratorConcept
  {
    void constraints() {
      function_requires< RandomAccessIteratorConcept<TT> >();
      function_requires< Mutable_BidirectionalIteratorConcept<TT> >();
      i[n] = *i;                  // require element access and assignment
    }
    TT i;
#ifndef BOOST_NO_STD_ITERATOR_TRAITS
    typename std::iterator_traits<TT>::difference_type n;
#else
    std::ptrdiff_t n;
#endif
  };

  //===========================================================================
  // Container Concepts

  template <class Container>
  struct ContainerConcept
  {
    typedef typename Container::value_type value_type;
    typedef typename Container::difference_type difference_type;
    typedef typename Container::size_type size_type;
    typedef typename Container::const_reference const_reference;
    typedef typename Container::const_pointer const_pointer;
    typedef typename Container::const_iterator const_iterator;

    void constraints() {
      function_requires< InputIteratorConcept<const_iterator> >();
      function_requires< AssignableConcept<Container> >();
      const_constraints(c);
    }
    void const_constraints(const Container& cc) {
      i = cc.begin();
      i = cc.end();
      n = cc.size();
      n = cc.max_size();
      b = cc.empty();
    }
    Container c;
    bool b;
    const_iterator i;
    size_type n;
  };

  template <class Container>
  struct Mutable_ContainerConcept
  {
    typedef typename Container::value_type value_type;
    typedef typename Container::reference reference;
    typedef typename Container::iterator iterator;
    typedef typename Container::pointer pointer;
    
    void constraints() {
      function_requires< ContainerConcept<Container> >();
      function_requires< AssignableConcept<value_type> >();
      function_requires< InputIteratorConcept<iterator> >();

      i = c.begin();
      i = c.end();
      c.swap(c2);
    }
    iterator i;
    Container c, c2;
  };

  template <class ForwardContainer>
  struct ForwardContainerConcept
  {
    void constraints() {
      function_requires< ContainerConcept<ForwardContainer> >();
      typedef typename ForwardContainer::const_iterator const_iterator;
      function_requires< ForwardIteratorConcept<const_iterator> >();
    }
  };  

  template <class ForwardContainer>
  struct Mutable_ForwardContainerConcept
  {
    void constraints() {
      function_requires< ForwardContainerConcept<ForwardContainer> >();
      function_requires< Mutable_ContainerConcept<ForwardContainer> >();
      typedef typename ForwardContainer::iterator iterator;
      function_requires< Mutable_ForwardIteratorConcept<iterator> >();
    }
  };  

  template <class ReversibleContainer>
  struct ReversibleContainerConcept
  {
    typedef typename ReversibleContainer::const_iterator const_iterator;
    typedef typename ReversibleContainer::const_reverse_iterator
      const_reverse_iterator;

    void constraints() {
      function_requires< ForwardContainerConcept<ReversibleContainer> >();
      function_requires< BidirectionalIteratorConcept<const_iterator> >();
      function_requires< 
        BidirectionalIteratorConcept<const_reverse_iterator> >();
      const_constraints(c);
    }
    void const_constraints(const ReversibleContainer& cc) {
      const_reverse_iterator i = cc.rbegin();
      i = cc.rend();
    }
    ReversibleContainer c;
  };

  template <class ReversibleContainer>
  struct Mutable_ReversibleContainerConcept
  {
    typedef typename ReversibleContainer::iterator iterator;
    typedef typename ReversibleContainer::reverse_iterator reverse_iterator;

    void constraints() {
      function_requires< ReversibleContainerConcept<ReversibleContainer> >();
      function_requires<
        Mutable_ForwardContainerConcept<ReversibleContainer> >();
      function_requires< Mutable_BidirectionalIteratorConcept<iterator> >();
      function_requires<
        Mutable_BidirectionalIteratorConcept<reverse_iterator> >();

      reverse_iterator i = c.rbegin();
      i = c.rend();
    }
    ReversibleContainer c;
  };

  template <class RandomAccessContainer>
  struct RandomAccessContainerConcept
  {
    typedef typename RandomAccessContainer::size_type size_type;
    typedef typename RandomAccessContainer::const_reference const_reference;
    typedef typename RandomAccessContainer::const_iterator const_iterator;
    typedef typename RandomAccessContainer::const_reverse_iterator
      const_reverse_iterator;

    void constraints() {
      function_requires< ReversibleContainerConcept<RandomAccessContainer> >();
      function_requires< RandomAccessIteratorConcept<const_iterator> >();
      function_requires<
        RandomAccessIteratorConcept<const_reverse_iterator> >();

      const_constraints(c);
    }
    void const_constraints(const RandomAccessContainer& cc) {
      const_reference r = cc[n];
      ignore_unused_variable_warning(r);
    }
    RandomAccessContainer c;
    size_type n;
  };

  template <class RandomAccessContainer>
  struct Mutable_RandomAccessContainerConcept
  {
    typedef typename RandomAccessContainer::size_type size_type;
    typedef typename RandomAccessContainer::reference reference;
    typedef typename RandomAccessContainer::iterator iterator;
    typedef typename RandomAccessContainer::reverse_iterator reverse_iterator;

    void constraints() {
      function_requires<
        RandomAccessContainerConcept<RandomAccessContainer> >();
      function_requires<
        Mutable_ReversibleContainerConcept<RandomAccessContainer> >();
      function_requires< Mutable_RandomAccessIteratorConcept<iterator> >();
      function_requires<
        Mutable_RandomAccessIteratorConcept<reverse_iterator> >();

      reference r = c[i];
      ignore_unused_variable_warning(r);
    }
    size_type i;
    RandomAccessContainer c;
  };

  // A Sequence is inherently mutable
  template <class Sequence>
  struct SequenceConcept
  {

    typedef typename Sequence::reference reference;
    typedef typename Sequence::const_reference const_reference;

    void constraints() {
      // Matt Austern's book puts DefaultConstructible here, the C++
      // standard places it in Container
      //    function_requires< DefaultConstructible<Sequence> >();
      function_requires< Mutable_ForwardContainerConcept<Sequence> >();
      function_requires< DefaultConstructibleConcept<Sequence> >();

      Sequence 
        c(n),
        c2(n, t),
        c3(first, last);

      c.insert(p, t);
      c.insert(p, n, t);
      c.insert(p, first, last);

      c.erase(p);
      c.erase(p, q);

      reference r = c.front();

      ignore_unused_variable_warning(c);
      ignore_unused_variable_warning(c2);
      ignore_unused_variable_warning(c3);
      ignore_unused_variable_warning(r);
      const_constraints(c);
    }
    void const_constraints(const Sequence& c) {
      const_reference r = c.front();
      ignore_unused_variable_warning(r);
    }
    typename Sequence::value_type t;
    typename Sequence::size_type n;
    typename Sequence::value_type* first, *last;
    typename Sequence::iterator p, q;
  };

  template <class FrontInsertionSequence>
  struct FrontInsertionSequenceConcept
  {
    void constraints() {
      function_requires< SequenceConcept<FrontInsertionSequence> >();

      c.push_front(t);
      c.pop_front();
    }
    FrontInsertionSequence c;
    typename FrontInsertionSequence::value_type t;
  };

  template <class BackInsertionSequence>
  struct BackInsertionSequenceConcept
  {
    typedef typename BackInsertionSequence::reference reference;
    typedef typename BackInsertionSequence::const_reference const_reference;

    void constraints() {
      function_requires< SequenceConcept<BackInsertionSequence> >();

      c.push_back(t);
      c.pop_back();
      reference r = c.back();
      ignore_unused_variable_warning(r);
    }
    void const_constraints(const BackInsertionSequence& cc) {
      const_reference r = cc.back();
      ignore_unused_variable_warning(r);
    };
    BackInsertionSequence c;
    typename BackInsertionSequence::value_type t;
  };

  template <class AssociativeContainer>
  struct AssociativeContainerConcept
  {
    void constraints() {
      function_requires< ForwardContainerConcept<AssociativeContainer> >();
      function_requires< DefaultConstructibleConcept<AssociativeContainer> >();
    
      i = c.find(k);
      r = c.equal_range(k);
      c.erase(k);
      c.erase(i);
      c.erase(r.first, r.second);
      const_constraints(c);
    }
    void const_constraints(const AssociativeContainer& cc) {
      ci = cc.find(k);
      n = cc.count(k);
      cr = cc.equal_range(k);
    }
    typedef typename AssociativeContainer::iterator iterator;
    typedef typename AssociativeContainer::const_iterator const_iterator;

    AssociativeContainer c;
    iterator i;
    std::pair<iterator,iterator> r;
    const_iterator ci;
    std::pair<const_iterator,const_iterator> cr;
    typename AssociativeContainer::key_type k;
    typename AssociativeContainer::size_type n;
  };

  template <class UniqueAssociativeContainer>
  struct UniqueAssociativeContainerConcept
  {
    void constraints() {
      function_requires< AssociativeContainerConcept<UniqueAssociativeContainer> >();
    
      UniqueAssociativeContainer c(first, last);
      
      pos_flag = c.insert(t);
      c.insert(first, last);

      ignore_unused_variable_warning(c);
    }
    std::pair<typename UniqueAssociativeContainer::iterator, bool> pos_flag;
    typename UniqueAssociativeContainer::value_type t;
    typename UniqueAssociativeContainer::value_type* first, *last;
  };

  template <class MultipleAssociativeContainer>
  struct MultipleAssociativeContainerConcept
  {
    void constraints() {
      function_requires< AssociativeContainerConcept<MultipleAssociativeContainer> >();

      MultipleAssociativeContainer c(first, last);
      
      pos = c.insert(t);
      c.insert(first, last);

      ignore_unused_variable_warning(c);
      ignore_unused_variable_warning(pos);
    }
    typename MultipleAssociativeContainer::iterator pos;
    typename MultipleAssociativeContainer::value_type t;
    typename MultipleAssociativeContainer::value_type* first, *last;
  };

  template <class SimpleAssociativeContainer>
  struct SimpleAssociativeContainerConcept
  {
    void constraints() {
      function_requires< AssociativeContainerConcept<SimpleAssociativeContainer> >();
      typedef typename SimpleAssociativeContainer::key_type key_type;
      typedef typename SimpleAssociativeContainer::value_type value_type;
      typedef typename require_same<key_type, value_type>::type req;
    }
  };

  template <class SimpleAssociativeContainer>
  struct PairAssociativeContainerConcept
  {
    void constraints() {
      function_requires< AssociativeContainerConcept<SimpleAssociativeContainer> >();
      typedef typename SimpleAssociativeContainer::key_type key_type;
      typedef typename SimpleAssociativeContainer::value_type value_type;
      typedef typename SimpleAssociativeContainer::mapped_type mapped_type;
      typedef std::pair<const key_type, mapped_type> required_value_type;
      typedef typename require_same<value_type, required_value_type>::type req;
    }
  };

  template <class SortedAssociativeContainer>
  struct SortedAssociativeContainerConcept
  {
    void constraints() {
      function_requires< AssociativeContainerConcept<SortedAssociativeContainer> >();
      function_requires< ReversibleContainerConcept<SortedAssociativeContainer> >();

      SortedAssociativeContainer 
        c(kc),
        c2(first, last),
        c3(first, last, kc);

      p = c.upper_bound(k);
      p = c.lower_bound(k);
      r = c.equal_range(k);
      
      c.insert(p, t);
      
      ignore_unused_variable_warning(c);
      ignore_unused_variable_warning(c2);
      ignore_unused_variable_warning(c3);
    }
    void const_constraints(const SortedAssociativeContainer& c) {
      kc = c.key_comp();
      vc = c.value_comp();

      cp = c.upper_bound(k);
      cp = c.lower_bound(k);
      cr = c.equal_range(k);
    }
    typename SortedAssociativeContainer::key_compare kc;
    typename SortedAssociativeContainer::value_compare vc;
    typename SortedAssociativeContainer::value_type t;
    typename SortedAssociativeContainer::key_type k;
    typedef typename SortedAssociativeContainer::iterator iterator;
    typedef typename SortedAssociativeContainer::const_iterator const_iterator;
    iterator p;
    const_iterator cp;
    std::pair<iterator,iterator> r;
    std::pair<const_iterator,const_iterator> cr;
    typename SortedAssociativeContainer::value_type* first, *last;
  };

  // HashedAssociativeContainer

} // namespace boost

#endif // BOOST_CONCEPT_CHECKS_HPP

