// Boost Lambda Library -- control_structures_impl.hpp ---------------------

// Copyright (C) 1999, 2000 Jaakko Järvi (jaakko.jarvi@cs.utu.fi)
// Copyright (C) 2000 Gary Powell (powellg@amazon.com)
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see www.boost.org

// --------------------------------------------------------------------------

#if !defined(BOOST_LAMBDA_CONTROL_CONSTRUCTS_HPP)
#define BOOST_LAMBDA_CONTROL_CONSTRUCTS_HPP

namespace boost { 
namespace lambda {

// -- void return control actions ----------------------

class forloop_action {};
class forloop_no_body_action {};
class ifthen_action {};
class ifthenelse_action {};
class whileloop_action {};
class whileloop_no_body_action {};
class dowhileloop_action {};
class dowhileloop_no_body_action {};
// -- nonvoid return control actions ----------------------

class ifthenelsereturn_action {};

// For loop
template <class Arg1, class Arg2, class Arg3, class Arg4>
inline const 
lambda_functor<
  lambda_functor_base<
    forloop_action, 
    tuple<lambda_functor<Arg1>, lambda_functor<Arg2>, 
          lambda_functor<Arg3>, lambda_functor<Arg4> >
  > 
>
for_loop(const lambda_functor<Arg1>& a1, const lambda_functor<Arg2>& a2, 
         const lambda_functor<Arg3>& a3, const lambda_functor<Arg4>& a4) { 
  return 
      lambda_functor_base<
        forloop_action, 
        tuple<lambda_functor<Arg1>, lambda_functor<Arg2>, 
              lambda_functor<Arg3>, lambda_functor<Arg4> >
      > 
    ( tuple<lambda_functor<Arg1>, lambda_functor<Arg2>, 
            lambda_functor<Arg3>, lambda_functor<Arg4> >(a1, a2, a3, a4)
    );
}

// No body case.
template <class Arg1, class Arg2, class Arg3>
inline const 
lambda_functor<
  lambda_functor_base<
    forloop_no_body_action, 
    tuple<lambda_functor<Arg1>, lambda_functor<Arg2>, lambda_functor<Arg3> >
  > 
>
for_loop(const lambda_functor<Arg1>& a1, const lambda_functor<Arg2>& a2, 
         const lambda_functor<Arg3>& a3) { 
  return 
      lambda_functor_base<
        forloop_no_body_action, 
        tuple<lambda_functor<Arg1>, lambda_functor<Arg2>, 
              lambda_functor<Arg3> >
      > 
      ( tuple<lambda_functor<Arg1>, lambda_functor<Arg2>, 
               lambda_functor<Arg3> >(a1, a2, a3) );
}

// While loop
template <class Arg1, class Arg2>
inline const 
lambda_functor<
  lambda_functor_base<
    whileloop_action, 
    tuple<lambda_functor<Arg1>, lambda_functor<Arg2> >
  > 
>
while_loop(const lambda_functor<Arg1>& a1, const lambda_functor<Arg2>& a2) { 
  return 
      lambda_functor_base<
        whileloop_action, 
        tuple<lambda_functor<Arg1>, lambda_functor<Arg2> >
      > 
      ( tuple<lambda_functor<Arg1>, lambda_functor<Arg2> >(a1, a2));
}

// No body case.
template <class Arg1>
inline const 
lambda_functor<
  lambda_functor_base<
    whileloop_no_body_action, 
    tuple<lambda_functor<Arg1> >
  > 
>
while_loop(const lambda_functor<Arg1>& a1) { 
  return 
      lambda_functor_base<
        whileloop_no_body_action, 
        tuple<lambda_functor<Arg1> >
      > 
      ( tuple<lambda_functor<Arg1> >(a1) );
}


// Do While loop
template <class Arg1, class Arg2>
inline const 
lambda_functor<
  lambda_functor_base<
    dowhileloop_action, 
    tuple<lambda_functor<Arg1>, lambda_functor<Arg2> >
  > 
>
do_while_loop(const lambda_functor<Arg1>& a1, const lambda_functor<Arg2>& a2) {
  return 
      lambda_functor_base<
        dowhileloop_action, 
        tuple<lambda_functor<Arg1>, lambda_functor<Arg2> >
      > 
      ( tuple<lambda_functor<Arg1>, lambda_functor<Arg2> >(a1, a2));
}

// No body case.
template <class Arg1>
inline const 
lambda_functor<
  lambda_functor_base<
    dowhileloop_no_body_action, 
    tuple<lambda_functor<Arg1> >
  > 
>
do_while_loop(const lambda_functor<Arg1>& a1) { 
  return 
      lambda_functor_base<
        dowhileloop_no_body_action, 
        tuple<lambda_functor<Arg1> >
      > 
      ( tuple<lambda_functor<Arg1> >(a1));
}


// If Then
template <class Arg1, class Arg2>
inline const 
lambda_functor<
  lambda_functor_base<
    ifthen_action, 
    tuple<lambda_functor<Arg1>, lambda_functor<Arg2> >
  > 
>
if_then(const lambda_functor<Arg1>& a1, const lambda_functor<Arg2>& a2) {
  return 
    lambda_functor_base<
      ifthen_action, 
      tuple<lambda_functor<Arg1>, lambda_functor<Arg2> > 
    > 
    ( tuple<lambda_functor<Arg1>, lambda_functor<Arg2> >(a1, a2) );
}

// If then else

template <class Arg1, class Arg2, class Arg3>
inline const 
lambda_functor<
  lambda_functor_base<
    ifthenelse_action, 
    tuple<lambda_functor<Arg1>, lambda_functor<Arg2>, lambda_functor<Arg3> >
  > 
>
if_then_else(const lambda_functor<Arg1>& a1, const lambda_functor<Arg2>& a2, 
             const lambda_functor<Arg3>& a3) {
  return 
    lambda_functor_base<
      ifthenelse_action, 
      tuple<lambda_functor<Arg1>, lambda_functor<Arg2>, lambda_functor<Arg3> >
    > 
    (tuple<lambda_functor<Arg1>, lambda_functor<Arg2>, lambda_functor<Arg3> >
       (a1, a2, a3) );
}

// Our version of operator?:()

template <class Arg1, class Arg2, class Arg3>
inline const 
  lambda_functor<
    lambda_functor_base<
      other_action<ifthenelsereturn_action>, 
      tuple<lambda_functor<Arg1>,
          typename const_copy_argument<Arg2>::type,
          typename const_copy_argument<Arg3>::type>
  > 
>
if_then_else_return(const lambda_functor<Arg1>& a1, 
                    const Arg2 & a2, 
                    const Arg3 & a3) {
  return 
      lambda_functor_base<
        other_action<ifthenelsereturn_action>, 
        tuple<lambda_functor<Arg1>,
              typename const_copy_argument<Arg2>::type,
              typename const_copy_argument<Arg3>::type>
      > ( tuple<lambda_functor<Arg1>,
              typename const_copy_argument<Arg2>::type,
              typename const_copy_argument<Arg3>::type> (a1, a2, a3) );
}

namespace detail {

// return type specialization for conditional expression begins -----------
// start reading below and move upwards

// PHASE 6:1 
// check if A is conbertible to B and B to A
template<int Phase, bool AtoB, bool BtoA, bool SameType, class A, class B>
struct return_type_2_ifthenelsereturn;

// if A can be converted to B and vice versa -> ambiguous
template<int Phase, class A, class B>
struct return_type_2_ifthenelsereturn<Phase, true, true, false, A, B> {
  typedef 
    detail::return_type_deduction_failure<return_type_2_ifthenelsereturn> type;
  // ambiguous type in conditional expression
};
// if A can be converted to B and vice versa and are of same type
template<int Phase, class A, class B>
struct return_type_2_ifthenelsereturn<Phase, true, true, true, A, B> {
  typedef A type;
};


// A can be converted to B
template<int Phase, class A, class B>
struct return_type_2_ifthenelsereturn<Phase, true, false, false, A, B> {
  typedef B type;
};

// B can be converted to A
template<int Phase, class A, class B>
struct return_type_2_ifthenelsereturn<Phase, false, true, false, A, B> {
  typedef A type;
};

// neither can be converted. Then we drop the potential references, and
// try again
template<class A, class B>
struct return_type_2_ifthenelsereturn<1, false, false, false, A, B> {
  // it is safe to add const, since the result will be an rvalue and thus
  // const anyway. The const are needed eg. if the types 
  // are 'const int*' and 'void *'. The remaining type should be 'const void*'
  typedef const typename boost::remove_reference<A>::type plainA; 
  typedef const typename boost::remove_reference<B>::type plainB; 
  // TODO: Add support for volatile ?

  typedef typename
       return_type_2_ifthenelsereturn<
         2,
         boost::is_convertible<plainA,plainB>::value, 
         boost::is_convertible<plainB,plainA>::value,
         boost::is_same<plainA,plainB>::value,
         plainA, 
         plainB>::type type;
};

// PHASE 6:2
template<class A, class B>
struct return_type_2_ifthenelsereturn<2, false, false, false, A, B> {
  typedef 
    detail::return_type_deduction_failure<return_type_2_ifthenelsereturn> type;
  // types_do_not_match_in_conditional_expression 
};



// PHASE 5: now we know that types are not arithmetic.
template<class A, class B>
struct non_numeric_types {
  typedef typename 
    return_type_2_ifthenelsereturn<
      1, // phase 1 
      is_convertible<A,B>::value, 
      is_convertible<B,A>::value, 
      is_same<A,B>::value,
      A, 
      B>::type type;
};

// PHASE 4 : 
// the base case covers arithmetic types with differing promote codes
// use the type deduction of arithmetic_actions
template<int CodeA, int CodeB, class A, class B>
struct arithmetic_or_not {
  typedef typename
    return_type_2<arithmetic_action<plus_action>, A, B>::type type; 
  // plus_action is just a random pick, has to be a concrete instance
};

// this case covers the case of artihmetic types with the same promote codes. 
// non numeric deduction is used since e.g. integral promotion is not 
// performed with operator ?: 
template<int CodeA, class A, class B>
struct arithmetic_or_not<CodeA, CodeA, A, B> {
  typedef typename non_numeric_types<A, B>::type type; 
};

// if either A or B has promote code -1 it is not an arithmetic type
template<class A, class B>
struct arithmetic_or_not <-1, -1, A, B> {
  typedef typename non_numeric_types<A, B>::type type;
};
template<int CodeB, class A, class B>
struct arithmetic_or_not <-1, CodeB, A, B> {
  typedef typename non_numeric_types<A, B>::type type;
};
template<int CodeA, class A, class B>
struct arithmetic_or_not <CodeA, -1, A, B> {
  typedef typename non_numeric_types<A, B>::type type;
};




// PHASE 3 : Are the types same?
// No, check if they are arithmetic or not
template <class A, class B>
struct same_or_not {
  typedef typename detail::remove_reference_and_cv<A>::type plainA;
  typedef typename detail::remove_reference_and_cv<B>::type plainB;

  typedef typename 
    arithmetic_or_not<
      detail::promote_code<plainA>::value, 
      detail::promote_code<plainB>::value, 
      A, 
      B>::type type;
};
// Yes, clear.
template <class A> struct same_or_not<A, A> {
  typedef A type;
};

} // detail

// PHASE 2 : Perform first the potential array_to_pointer conversion 
template<class A, class B>
struct return_type_2<other_action<ifthenelsereturn_action>, A, B> { 

  typedef typename detail::array_to_pointer<A>::type A1;
  typedef typename detail::array_to_pointer<B>::type B1;

  typedef typename 
    boost::add_const<typename detail::same_or_not<A1, B1>::type>::type type;
};

// PHASE 1 : Deduction is based on the second and third operand


// return type specialization for conditional expression ends -----------




// Control loop lambda_functor_base specializations.

// Specialization for for_loop.
template<class Args>
class 
lambda_functor_base<forloop_action, Args> {
public:
  Args args;
  template <class T> struct sig { typedef void type; };
public:
  explicit lambda_functor_base(const Args& a) : args(a) {}

  template<class RET, CALL_TEMPLATE_ARGS>
  RET call(CALL_FORMAL_ARGS) const {
    for(detail::select(boost::tuples::get<0>(args), CALL_ACTUAL_ARGS); 
        detail::select(boost::tuples::get<1>(args), CALL_ACTUAL_ARGS); 
        detail::select(boost::tuples::get<2>(args), CALL_ACTUAL_ARGS))
      
      detail::select(boost::tuples::get<3>(args), CALL_ACTUAL_ARGS);
  }
};

// No body case
template<class Args>
class 
lambda_functor_base<forloop_no_body_action, Args> {
public:
  Args args;
  template <class T> struct sig { typedef void type; };
public:
  explicit lambda_functor_base(const Args& a) : args(a) {}

  template<class RET, CALL_TEMPLATE_ARGS>
  RET call(CALL_FORMAL_ARGS) const {
    for(detail::select(boost::tuples::get<0>(args), CALL_ACTUAL_ARGS); 
        detail::select(boost::tuples::get<1>(args), CALL_ACTUAL_ARGS); 
        detail::select(boost::tuples::get<2>(args), CALL_ACTUAL_ARGS)) {}
   }
};


// Specialization for while_loop.
template<class Args>
class 
lambda_functor_base<whileloop_action, Args> {
public:
  Args args;
  template <class T> struct sig { typedef void type; };
public:
  explicit lambda_functor_base(const Args& a) : args(a) {}

  template<class RET, CALL_TEMPLATE_ARGS>
  RET call(CALL_FORMAL_ARGS) const {
    while(detail::select(boost::tuples::get<0>(args), CALL_ACTUAL_ARGS))
      
      detail::select(boost::tuples::get<1>(args), CALL_ACTUAL_ARGS);
  }
};

// No body case
template<class Args> 
class 
lambda_functor_base<whileloop_no_body_action, Args> {
public:
  Args args;
  template <class T> struct sig { typedef void type; };
public:
  explicit lambda_functor_base(const Args& a) : args(a) {}

  template<class RET, CALL_TEMPLATE_ARGS>
  RET call(CALL_FORMAL_ARGS) const {
          while(detail::select(boost::tuples::get<0>(args), CALL_ACTUAL_ARGS)) {}
  }
};

// Specialization for do_while_loop.
// Note that the first argument is the condition.
template<class Args>
class 
lambda_functor_base<dowhileloop_action, Args> {
public:
  Args args;
  template <class T> struct sig { typedef void type; };
public:
  explicit lambda_functor_base(const Args& a) : args(a) {}

  template<class RET, CALL_TEMPLATE_ARGS>
  RET call(CALL_FORMAL_ARGS) const {
    do {
      detail::select(boost::tuples::get<1>(args), CALL_ACTUAL_ARGS);      
    } while (detail::select(boost::tuples::get<0>(args), CALL_ACTUAL_ARGS) );
  }
};

// No body case
template<class Args>
class 
lambda_functor_base<dowhileloop_no_body_action, Args> {
public:
  Args args;
  template <class T> struct sig { typedef void type; };
public:
  explicit lambda_functor_base(const Args& a) : args(a) {}

  template<class RET, CALL_TEMPLATE_ARGS>
  RET call(CALL_FORMAL_ARGS) const {
          do {} while (detail::select(boost::tuples::get<0>(args), CALL_ACTUAL_ARGS) );
  }
};


// Specialization for if_then.
template<class Args>
class 
lambda_functor_base<ifthen_action, Args> {
public:
  Args args;
  template <class T> struct sig { typedef void type; };
public:
  explicit lambda_functor_base(const Args& a) : args(a) {}

  template<class RET, CALL_TEMPLATE_ARGS>
  RET call(CALL_FORMAL_ARGS) const {
    if (detail::select(boost::tuples::get<0>(args), CALL_ACTUAL_ARGS)) detail::select(boost::tuples::get<1>(args), CALL_ACTUAL_ARGS); 
  }
};

// Specialization for if_then_else.
template<class Args>
class 
lambda_functor_base<ifthenelse_action, Args> {
public:
  Args args;
  template <class T> struct sig { typedef void type; };
public:
  explicit lambda_functor_base(const Args& a) : args(a) {}

  template<class RET, CALL_TEMPLATE_ARGS>
  RET call(CALL_FORMAL_ARGS) const {
    if (detail::select(boost::tuples::get<0>(args), CALL_ACTUAL_ARGS)) 
      detail::select(boost::tuples::get<1>(args), CALL_ACTUAL_ARGS); 
    else 
      detail::select(boost::tuples::get<2>(args), CALL_ACTUAL_ARGS);
  }
};

// Specialization of lambda_functor_base for if_then_else_return.
template<class Args>
class 
lambda_functor_base<other_action<ifthenelsereturn_action>, Args> {
public:
  Args args;

  template <class SigArgs> struct sig {
  private:
    typedef typename detail::nth_return_type_sig<1, Args, SigArgs>::type ret1;
    typedef typename detail::nth_return_type_sig<2, Args, SigArgs>::type ret2;
  public:
    typedef typename return_type_2<
      other_action<ifthenelsereturn_action>, ret1, ret2
    >::type type;
  };

public:
  explicit lambda_functor_base(const Args& a) : args(a) {}

  template<class RET, CALL_TEMPLATE_ARGS>
  RET call(CALL_FORMAL_ARGS) const {
    return (detail::select(boost::tuples::get<0>(args), CALL_ACTUAL_ARGS)) ?
       detail::select(boost::tuples::get<1>(args), CALL_ACTUAL_ARGS) 
    : 
       detail::select(boost::tuples::get<2>(args), CALL_ACTUAL_ARGS);
  }
};

} // lambda
} // boost

#endif // BOOST_LAMBDA_CONTROL_CONSTRUCTS_HPP
