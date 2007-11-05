// (C) Copyright Jeremy Siek 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_ITERATOR_CONCEPTS_HPP
#define BOOST_ITERATOR_CONCEPTS_HPP

//  Revision History
//  26 Apr 2003 thw
//       Adapted to new iterator concepts
//  22 Nov 2002 Thomas Witt
//       Added interoperable concept.

#include <boost/concept_check.hpp>
#include <boost/iterator/iterator_categories.hpp>

// Use boost::detail::iterator_traits to work around some MSVC/Dinkumware problems.
#include <boost/detail/iterator.hpp>

#include <boost/type_traits/is_same.hpp>
#include <boost/type_traits/is_integral.hpp>
#include <boost/type_traits/is_convertible.hpp>

#include <boost/mpl/bool.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/and.hpp>
#include <boost/mpl/or.hpp>

#include <boost/static_assert.hpp>

// Use boost/limits to work around missing limits headers on some compilers
#include <boost/limits.hpp>
#include <boost/config.hpp>

#include <algorithm>

namespace boost_concepts {
  // Used a different namespace here (instead of "boost") so that the
  // concept descriptions do not take for granted the names in
  // namespace boost.

  // We use this in place of STATIC_ASSERT((is_convertible<...>))
  // because some compilers (CWPro7.x) can't detect convertibility.
  //
  // Of course, that just gets us a different error at the moment with
  // some tests, since new iterator category deduction still depends
  // on convertibility detection. We might need some specializations
  // to support this compiler.
  template <class Target, class Source>
  struct static_assert_base_and_derived
  {
      static_assert_base_and_derived(Target* = (Source*)0) {}
  };

  //===========================================================================
  // Iterator Access Concepts

  template <typename Iterator>
  class ReadableIteratorConcept {
  public:
    typedef BOOST_DEDUCED_TYPENAME boost::detail::iterator_traits<Iterator>::value_type value_type;

    void constraints() {
      boost::function_requires< boost::AssignableConcept<Iterator> >();
      boost::function_requires< boost::CopyConstructibleConcept<Iterator> >();

      value_type v = *i;
      boost::ignore_unused_variable_warning(v);
    }
    Iterator i;
  };
  
  template <
      typename Iterator
    , typename ValueType = BOOST_DEDUCED_TYPENAME boost::detail::iterator_traits<Iterator>::value_type
  >
  class WritableIteratorConcept {
  public:
      
    void constraints() {
      boost::function_requires< boost::CopyConstructibleConcept<Iterator> >();
      *i = v;
    }
    ValueType v;
    Iterator i;
  };
  
  template <typename Iterator>
  class SwappableIteratorConcept {
  public:

    void constraints() {
      std::iter_swap(i1, i2);
    }
    Iterator i1;
    Iterator i2;
  };

  template <typename Iterator>
  class LvalueIteratorConcept
  {
   public:
      typedef typename boost::detail::iterator_traits<Iterator>::value_type value_type;
      void constraints()
      {
        value_type& r = const_cast<value_type&>(*i);
        boost::ignore_unused_variable_warning(r);
      }
    Iterator i;
  };

  
  //===========================================================================
  // Iterator Traversal Concepts

  template <typename Iterator>
  class IncrementableIteratorConcept {
  public:
    typedef typename boost::iterator_traversal<Iterator>::type traversal_category;

    void constraints() {
      boost::function_requires< boost::AssignableConcept<Iterator> >();
      boost::function_requires< boost::CopyConstructibleConcept<Iterator> >();

      BOOST_STATIC_ASSERT(
          (boost::is_convertible<
                traversal_category
              , boost::incrementable_traversal_tag
           >::value
          ));

      ++i;
      (void)i++;
    }
    Iterator i;
  };

  template <typename Iterator>
  class SinglePassIteratorConcept {
  public:
    typedef typename boost::iterator_traversal<Iterator>::type traversal_category;
    typedef typename boost::detail::iterator_traits<Iterator>::difference_type difference_type;

    void constraints() {
      boost::function_requires< IncrementableIteratorConcept<Iterator> >();
      boost::function_requires< boost::EqualityComparableConcept<Iterator> >();

      BOOST_STATIC_ASSERT(
          (boost::is_convertible<
                traversal_category
              , boost::single_pass_traversal_tag
           >::value
          ));
    }
  };

  template <typename Iterator>
  class ForwardTraversalConcept {
  public:
    typedef typename boost::iterator_traversal<Iterator>::type traversal_category;
    typedef typename boost::detail::iterator_traits<Iterator>::difference_type difference_type;

    void constraints() {
      boost::function_requires< SinglePassIteratorConcept<Iterator> >();
      boost::function_requires< 
        boost::DefaultConstructibleConcept<Iterator> >();

      typedef boost::mpl::and_<
        boost::is_integral<difference_type>,
        boost::mpl::bool_< std::numeric_limits<difference_type>::is_signed >
        > difference_type_is_signed_integral;

      BOOST_STATIC_ASSERT(difference_type_is_signed_integral::value);
      BOOST_STATIC_ASSERT(
          (boost::is_convertible<
                traversal_category
              , boost::forward_traversal_tag
           >::value
          ));
    }
  };
  
  template <typename Iterator>
  class BidirectionalTraversalConcept {
  public:
    typedef typename boost::iterator_traversal<Iterator>::type traversal_category;

    void constraints() {
      boost::function_requires< ForwardTraversalConcept<Iterator> >();
      
      BOOST_STATIC_ASSERT(
          (boost::is_convertible<
                traversal_category
              , boost::bidirectional_traversal_tag
           >::value
          ));

      --i;
      (void)i--;
    }
    Iterator i;
  };

  template <typename Iterator>
  class RandomAccessTraversalConcept {
  public:
    typedef typename boost::iterator_traversal<Iterator>::type traversal_category;
    typedef typename boost::detail::iterator_traits<Iterator>::difference_type
      difference_type;

    void constraints() {
      boost::function_requires< BidirectionalTraversalConcept<Iterator> >();

      BOOST_STATIC_ASSERT(
          (boost::is_convertible<
                traversal_category
              , boost::random_access_traversal_tag
           >::value
          ));
      
      i += n;
      i = i + n;
      i = n + i;
      i -= n;
      i = i - n;
      n = i - j;
    }
    difference_type n;
    Iterator i, j;
  };

  //===========================================================================
  // Iterator Interoperability Concept

  namespace detail
  {

    template <typename Iterator1, typename Iterator2>
    void interop_single_pass_constraints(Iterator1 const& i1, Iterator2 const& i2)
    {
      bool b;
      b = i1 == i2;
      b = i1 != i2;
      
      b = i2 == i1;
      b = i2 != i1;
    }
    
    template <typename Iterator1, typename Iterator2>
    void interop_rand_access_constraints(Iterator1 const& i1, Iterator2 const& i2,
                                         boost::random_access_traversal_tag, boost::random_access_traversal_tag)
    {
      bool b;
      typename boost::detail::iterator_traits<Iterator2>::difference_type n;
      b = i1 <  i2;
      b = i1 <= i2;
      b = i1 >  i2;
      b = i1 >= i2;
      n = i1 -  i2;
      
      b = i2 <  i1;
      b = i2 <= i1;
      b = i2 >  i1;
      b = i2 >= i1;
      n = i2 -  i1;
    }
    template <typename Iterator1, typename Iterator2>
    void interop_rand_access_constraints(Iterator1 const& i1, Iterator2 const& i2,
                                         boost::single_pass_traversal_tag, boost::single_pass_traversal_tag)
    { }

  } // namespace detail

  template <typename Iterator, typename ConstIterator>
  class InteroperableIteratorConcept
  {
  public:
      typedef typename boost::detail::pure_traversal_tag<
          typename boost::iterator_traversal<
              Iterator
          >::type
      >::type traversal_category;

      typedef typename boost::detail::pure_traversal_tag<
          typename boost::iterator_traversal<
              ConstIterator
          >::type
      >::type const_traversal_category;

      void constraints()
      {
          boost::function_requires< SinglePassIteratorConcept<Iterator> >();
          boost::function_requires< SinglePassIteratorConcept<ConstIterator> >();

          detail::interop_single_pass_constraints(i, ci);
          detail::interop_rand_access_constraints(i, ci, traversal_category(), const_traversal_category());

          ci = i;
      }
      Iterator      i;
      ConstIterator ci;
  };

} // namespace boost_concepts


#endif // BOOST_ITERATOR_CONCEPTS_HPP
