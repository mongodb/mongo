// (C) Copyright David Abrahams 2002.
// (C) Copyright Jeremy Siek    2002.
// (C) Copyright Thomas Witt    2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_TRANSFORM_ITERATOR_23022003THW_HPP
#define BOOST_TRANSFORM_ITERATOR_23022003THW_HPP

#include <boost/function.hpp>
#include <boost/iterator.hpp>
#include <boost/iterator/detail/enable_if.hpp>
#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/iterator/iterator_categories.hpp>
#include <boost/mpl/not.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/type_traits/function_traits.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/type_traits/is_class.hpp>
#include <boost/type_traits/is_function.hpp>
#include <boost/type_traits/is_reference.hpp>
#include <boost/type_traits/remove_const.hpp>
#include <boost/type_traits/remove_reference.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC, BOOST_TESTED_AT(1310))
# include <boost/type_traits/is_base_and_derived.hpp>

#endif 
#include <boost/iterator/detail/config_def.hpp>


namespace boost
{
  template <class UnaryFunction, class Iterator, class Reference = use_default, class Value = use_default>
  class transform_iterator;

  namespace detail 
  {

    template <class UnaryFunction>
    struct function_object_result
    {
      typedef typename UnaryFunction::result_type type;
    };

#ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
    template <class Return, class Argument>
    struct function_object_result<Return(*)(Argument)>
    {
      typedef Return type;
    };
#endif

    // Compute the iterator_adaptor instantiation to be used for transform_iterator
    template <class UnaryFunction, class Iterator, class Reference, class Value>
    struct transform_iterator_base
    {
     private:
        // By default, dereferencing the iterator yields the same as
        // the function.  Do we need to adjust the way
        // function_object_result is computed for the standard
        // proposal (e.g. using Doug's result_of)?
        typedef typename ia_dflt_help<
            Reference
          , function_object_result<UnaryFunction>
        >::type reference;

        // To get the default for Value: remove any reference on the
        // result type, but retain any constness to signal
        // non-writability.  Note that if we adopt Thomas' suggestion
        // to key non-writability *only* on the Reference argument,
        // we'd need to strip constness here as well.
        typedef typename ia_dflt_help<
            Value
          , remove_reference<reference>
        >::type cv_value_type;

     public:
        typedef iterator_adaptor<
            transform_iterator<UnaryFunction, Iterator, Reference, Value>
          , Iterator
          , cv_value_type
          , use_default    // Leave the traversal category alone
          , reference
        > type;
    };
  }

  template <class UnaryFunction, class Iterator, class Reference, class Value>
  class transform_iterator
    : public detail::transform_iterator_base<UnaryFunction, Iterator, Reference, Value>::type
  {
    typedef typename
    detail::transform_iterator_base<UnaryFunction, Iterator, Reference, Value>::type
    super_t;

    friend class iterator_core_access;

  public:
    transform_iterator() { }

    transform_iterator(Iterator const& x, UnaryFunction f)
      : super_t(x), m_f(f) { }

    explicit transform_iterator(Iterator const& x)
      : super_t(x)
    {
        // Pro8 is a little too aggressive about instantiating the
        // body of this function.
#if !BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3003))
        // don't provide this constructor if UnaryFunction is a
        // function pointer type, since it will be 0.  Too dangerous.
        BOOST_STATIC_ASSERT(is_class<UnaryFunction>::value);
#endif 
    }

    template<
        class OtherUnaryFunction
      , class OtherIterator
      , class OtherReference
      , class OtherValue>
    transform_iterator(
         transform_iterator<OtherUnaryFunction, OtherIterator, OtherReference, OtherValue> const& t
       , typename enable_if_convertible<OtherIterator, Iterator>::type* = 0
#if !BOOST_WORKAROUND(BOOST_MSVC, == 1310)
       , typename enable_if_convertible<OtherUnaryFunction, UnaryFunction>::type* = 0
#endif 
    )
      : super_t(t.base()), m_f(t.functor())
   {}

    UnaryFunction functor() const
      { return m_f; }

  private:
    typename super_t::reference dereference() const
    { return m_f(*this->base()); }

    // Probably should be the initial base class so it can be
    // optimized away via EBO if it is an empty class.
    UnaryFunction m_f;
  };

  template <class UnaryFunction, class Iterator>
  transform_iterator<UnaryFunction, Iterator>
  make_transform_iterator(Iterator it, UnaryFunction fun)
  {
      return transform_iterator<UnaryFunction, Iterator>(it, fun);
  }

  // Version which allows explicit specification of the UnaryFunction
  // type.
  //
  // This generator is not provided if UnaryFunction is a function
  // pointer type, because it's too dangerous: the default-constructed
  // function pointer in the iterator be 0, leading to a runtime
  // crash.
  template <class UnaryFunction, class Iterator>
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
  typename mpl::if_<
#else 
  typename iterators::enable_if<
#endif 
      is_class<UnaryFunction>   // We should probably find a cheaper test than is_class<>
    , transform_iterator<UnaryFunction, Iterator>
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
    , int[3]
#endif 
  >::type
  make_transform_iterator(Iterator it)
  {
      return transform_iterator<UnaryFunction, Iterator>(it, UnaryFunction());
  }

#if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION ) && !defined(BOOST_NO_FUNCTION_TEMPLATE_ORDERING)
  template <class Return, class Argument, class Iterator>
  transform_iterator< Return (*)(Argument), Iterator, Return>
  make_transform_iterator(Iterator it, Return (*fun)(Argument))
  {
    return transform_iterator<Return (*)(Argument), Iterator, Return>(it, fun);
  }
#endif

} // namespace boost

#include <boost/iterator/detail/config_undef.hpp>

#endif // BOOST_TRANSFORM_ITERATOR_23022003THW_HPP
