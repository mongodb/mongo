// Boost Lambda Library -  lambda_functors.hpp -------------------------------

// Copyright (C) 1999, 2000 Jaakko Järvi (jaakko.jarvi@cs.utu.fi)
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org

// ------------------------------------------------

#ifndef BOOST_LAMBDA_LAMBDA_FUNCTORS_HPP
#define BOOST_LAMBDA_LAMBDA_FUNCTORS_HPP

namespace boost { 
namespace lambda {

// -- lambda_functor --------------------------------------------
// --------------------------------------------------------------

//inline const null_type const_null_type() { return null_type(); }

namespace detail {
namespace {

  static const null_type constant_null_type = null_type();

} // unnamed
} // detail

class unused {};

#define cnull_type() detail::constant_null_type

// -- free variables types -------------------------------------------------- 
 
  // helper to work around the case where the nullary return type deduction 
  // is always performed, even though the functor is not nullary  
namespace detail {
  template<int N, class Tuple> struct get_element_or_null_type {
    typedef typename 
      detail::tuple_element_as_reference<N, Tuple>::type type;
  };
  template<int N> struct get_element_or_null_type<N, null_type> {
    typedef null_type type;
  };
}

template <int I> struct placeholder;

template<> struct placeholder<FIRST> {

  template<class SigArgs> struct sig {
    typedef typename detail::get_element_or_null_type<0, SigArgs>::type type;
  };

  template<class RET, CALL_TEMPLATE_ARGS> 
  RET call(CALL_FORMAL_ARGS) const { 
    BOOST_STATIC_ASSERT(boost::is_reference<RET>::value); 
    CALL_USE_ARGS; // does nothing, prevents warnings for unused args
    return a; 
  }
};

template<> struct placeholder<SECOND> {

  template<class SigArgs> struct sig {
    typedef typename detail::get_element_or_null_type<1, SigArgs>::type type;
  };

  template<class RET, CALL_TEMPLATE_ARGS> 
  RET call(CALL_FORMAL_ARGS) const { CALL_USE_ARGS; return b; }
};

template<> struct placeholder<THIRD> {

  template<class SigArgs> struct sig {
    typedef typename detail::get_element_or_null_type<2, SigArgs>::type type;
  };

  template<class RET, CALL_TEMPLATE_ARGS> 
  RET call(CALL_FORMAL_ARGS) const { CALL_USE_ARGS; return c; }
};

template<> struct placeholder<EXCEPTION> {

  template<class SigArgs> struct sig {
    typedef typename detail::get_element_or_null_type<3, SigArgs>::type type;
  };

  template<class RET, CALL_TEMPLATE_ARGS> 
  RET call(CALL_FORMAL_ARGS) const { CALL_USE_ARGS; return env; }
};
   
typedef const lambda_functor<placeholder<FIRST> >  placeholder1_type;
typedef const lambda_functor<placeholder<SECOND> > placeholder2_type;
typedef const lambda_functor<placeholder<THIRD> >  placeholder3_type;
   

///////////////////////////////////////////////////////////////////////////////


// free variables are lambda_functors. This is to allow uniform handling with 
// other lambda_functors.
// -------------------------------------------------------------------



// -- lambda_functor NONE ------------------------------------------------
template <class T>
class lambda_functor : public T 
{

BOOST_STATIC_CONSTANT(int, arity_bits = get_arity<T>::value);
 
public:
  typedef T inherited;

  lambda_functor() {}
  lambda_functor(const lambda_functor& l) : inherited(l) {}

  lambda_functor(const T& t) : inherited(t) {}

  template <class SigArgs> struct sig {
    typedef typename inherited::template 
      sig<typename SigArgs::tail_type>::type type;
  };

  // Note that this return type deduction template is instantiated, even 
  // if the nullary 
  // operator() is not called at all. One must make sure that it does not fail.
  typedef typename 
    inherited::template sig<null_type>::type
      nullary_return_type;

  nullary_return_type operator()() const { 
    return inherited::template 
      call<nullary_return_type>
        (cnull_type(), cnull_type(), cnull_type(), cnull_type()); 
  }

  template<class A>
  typename inherited::template sig<tuple<A&> >::type
  operator()(A& a) const { 
    return inherited::template call<
      typename inherited::template sig<tuple<A&> >::type
    >(a, cnull_type(), cnull_type(), cnull_type());
  }

  template<class A, class B>
  typename inherited::template sig<tuple<A&, B&> >::type
  operator()(A& a, B& b) const { 
    return inherited::template call<
      typename inherited::template sig<tuple<A&, B&> >::type
    >(a, b, cnull_type(), cnull_type()); 
  }

  template<class A, class B, class C>
  typename inherited::template sig<tuple<A&, B&, C&> >::type
  operator()(A& a, B& b, C& c) const
  { 
    return inherited::template call<
      typename inherited::template sig<tuple<A&, B&, C&> >::type
    >(a, b, c, cnull_type()); 
  }

  // for internal calls with env
  template<CALL_TEMPLATE_ARGS>
  typename inherited::template sig<tuple<CALL_REFERENCE_TYPES> >::type
  internal_call(CALL_FORMAL_ARGS) const { 
     return inherited::template 
       call<typename inherited::template 
         sig<tuple<CALL_REFERENCE_TYPES> >::type>(CALL_ACTUAL_ARGS); 
  }

  template<class A>
  const lambda_functor<lambda_functor_base<
                  other_action<assignment_action>,
                  boost::tuple<lambda_functor,
                  typename const_copy_argument <const A>::type> > >
  operator=(const A& a) const {
    return lambda_functor_base<
                  other_action<assignment_action>,
                  boost::tuple<lambda_functor,
                  typename const_copy_argument <const A>::type> >
     (  boost::tuple<lambda_functor,
             typename const_copy_argument <const A>::type>(*this, a) );
  }

  template<class A> 
  const lambda_functor<lambda_functor_base< 
                  other_action<subscript_action>, 
                  boost::tuple<lambda_functor, 
                        typename const_copy_argument <const A>::type> > > 
  operator[](const A& a) const { 
    return lambda_functor_base< 
                  other_action<subscript_action>, 
                  boost::tuple<lambda_functor, 
                        typename const_copy_argument <const A>::type> >
     ( boost::tuple<lambda_functor, 
             typename const_copy_argument <const A>::type>(*this, a ) ); 
  } 
};


} // namespace lambda
} // namespace boost

#endif


