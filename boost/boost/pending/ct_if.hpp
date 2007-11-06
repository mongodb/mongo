// (C) Copyright Jeremy Siek 2000.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// The ct_if implementation that avoids partial specialization is
// based on the IF class by Ulrich W. Eisenecker and Krzysztof
// Czarnecki.

#ifndef BOOST_CT_IF_HPP
#define BOOST_CT_IF_HPP

#include <boost/config.hpp>

/*
  There is a bug in the Borland compiler with regards to using
  integers to specialize templates. This made it hard to use ct_if in
  the graph library. Changing from 'ct_if' to 'ct_if_t' fixed the
  problem.
*/

#include <boost/type_traits/integral_constant.hpp> // true_type and false_type

namespace boost {

  struct ct_if_error { };

  template <class A, class B>
  struct ct_and { typedef false_type type; };
  template <> struct ct_and<true_type,true_type> { typedef true_type type; };

  template <class A> struct ct_not { typedef ct_if_error type; };
  template <> struct ct_not<true_type> { typedef false_type type; };
  template <> struct ct_not<false_type> { typedef true_type type; };

#ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

// agurt, 15/sep/02: in certain cases Borland has problems with
// choosing the right 'ct_if' specialization even though 'cond' 
// _does_ equal '1'; the easiest way to fix it is to make first 
// 'ct_if' non-type template parameter boolean.
#if !defined(__BORLANDC__)
  template <bool cond, class A, class B>
  struct ct_if { typedef ct_if_error type; };
  template <class A, class B>
  struct ct_if<true, A, B> { typedef A type; };
  template <class A, class B>
  struct ct_if<false, A, B> { typedef B type; };
#else
  template <bool cond, class A, class B>
  struct ct_if { typedef A type; };
  template <class A, class B>
  struct ct_if<false, A, B> { typedef B type; };
#endif

  template <class cond, class A, class B>
  struct ct_if_t { typedef ct_if_error type; };
  template <class A, class B>
  struct ct_if_t<true_type, A, B> { typedef A type; };
  template <class A, class B>
  struct ct_if_t<false_type, A, B> { typedef B type; };

#else

  namespace detail {

    template <int condition, class A, class B> struct IF;
    template <int condition> struct SlectSelector;
    struct SelectFirstType;
    struct SelectSecondType;
    
    struct SelectFirstType {
      template<class A, class B>
      struct Template {        typedef A type; };
    };
    
    struct SelectSecondType {
      template<class A, class B>
      struct Template { typedef B type; };
    };
    
    template<int condition>
    struct SlectSelector {
      typedef SelectFirstType type;
    };
    
    template <>
    struct SlectSelector<0> {
      typedef SelectSecondType type;
    };

  } // namespace detail
    
  template<int condition, class A, class B>
  struct ct_if
  {
    typedef typename detail::SlectSelector<condition>::type Selector;
    typedef typename Selector::template Template<A, B>::type type;
  };
  
  template <class cond, class A, class B>
  struct ct_if_t { 
    typedef typename ct_if<cond::value, A, B>::type type;
  };

#endif

} // namespace boost

#endif // BOOST_CT_IF_HPP

