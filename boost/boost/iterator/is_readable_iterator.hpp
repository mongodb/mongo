// Copyright David Abrahams 2003. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef IS_READABLE_ITERATOR_DWA2003112_HPP
# define IS_READABLE_ITERATOR_DWA2003112_HPP

#include <boost/mpl/bool.hpp>
#include <boost/detail/iterator.hpp>

#include <boost/type_traits/detail/bool_trait_def.hpp>
#include <boost/iterator/detail/any_conversion_eater.hpp>

// should be the last #include
#include <boost/iterator/detail/config_def.hpp>

#ifndef BOOST_NO_IS_CONVERTIBLE

namespace boost {
 
namespace detail
{
  // Guts of is_readable_iterator.  Value is the iterator's value_type
  // and the result is computed in the nested rebind template.
  template <class Value>
  struct is_readable_iterator_impl
  {
      static char tester(Value&, int);
      static char (& tester(any_conversion_eater, ...) )[2];
    
      template <class It>
      struct rebind
      {
          static It& x;
          
          BOOST_STATIC_CONSTANT(
              bool
            , value = (
                sizeof(
                    is_readable_iterator_impl<Value>::tester(*x, 1)
                ) == 1
            )
          );
      };
  };

#undef BOOST_READABLE_PRESERVER
  
  //
  // void specializations to handle std input and output iterators
  //
  template <>
  struct is_readable_iterator_impl<void>
  {
      template <class It>
      struct rebind : boost::mpl::false_
      {};
  };

#ifndef BOOST_NO_CV_VOID_SPECIALIZATIONS
  template <>
  struct is_readable_iterator_impl<const void>
  {
      template <class It>
      struct rebind : boost::mpl::false_
      {};
  };

  template <>
  struct is_readable_iterator_impl<volatile void>
  {
      template <class It>
      struct rebind : boost::mpl::false_
      {};
  };

  template <>
  struct is_readable_iterator_impl<const volatile void>
  {
      template <class It>
      struct rebind : boost::mpl::false_
      {};
  };
#endif

  //
  // This level of dispatching is required for Borland.  We might save
  // an instantiation by removing it for others.
  //
  template <class It>
  struct is_readable_iterator_impl2
    : is_readable_iterator_impl<
          BOOST_DEDUCED_TYPENAME boost::detail::iterator_traits<It>::value_type const
      >::template rebind<It>
  {};
} // namespace detail

// Define the trait with full mpl lambda capability and various broken
// compiler workarounds
BOOST_TT_AUX_BOOL_TRAIT_DEF1(
    is_readable_iterator,T,::boost::detail::is_readable_iterator_impl2<T>::value)
    
} // namespace boost

#endif

#include <boost/iterator/detail/config_undef.hpp>

#endif // IS_READABLE_ITERATOR_DWA2003112_HPP
