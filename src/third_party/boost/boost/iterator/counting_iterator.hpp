// Copyright David Abrahams 2003.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef COUNTING_ITERATOR_DWA200348_HPP
# define COUNTING_ITERATOR_DWA200348_HPP

# include <boost/config.hpp>
# include <boost/static_assert.hpp>
# ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
# include <limits>
# elif !BOOST_WORKAROUND(BOOST_BORLANDC, BOOST_TESTED_AT(0x551))
# include <boost/type_traits/is_convertible.hpp>
# else
# include <boost/type_traits/is_arithmetic.hpp>
# endif
# include <boost/type_traits/is_integral.hpp>
# include <boost/type_traits/type_identity.hpp>
# include <boost/type_traits/conditional.hpp>
# include <boost/type_traits/integral_constant.hpp>
# include <boost/detail/numeric_traits.hpp>
# include <boost/iterator/iterator_adaptor.hpp>

namespace boost {
namespace iterators {

template <
    class Incrementable
  , class CategoryOrTraversal
  , class Difference
>
class counting_iterator;

namespace detail
{
  // Try to detect numeric types at compile time in ways compatible
  // with the limitations of the compiler and library.
  template <class T>
  struct is_numeric_impl
  {
      // For a while, this wasn't true, but we rely on it below. This is a regression assert.
      BOOST_STATIC_ASSERT(::boost::is_integral<char>::value);

# ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS

      BOOST_STATIC_CONSTANT(bool, value = std::numeric_limits<T>::is_specialized);

# else

#  if !BOOST_WORKAROUND(BOOST_BORLANDC, BOOST_TESTED_AT(0x551))
      BOOST_STATIC_CONSTANT(
          bool, value = (
              boost::is_convertible<int,T>::value
           && boost::is_convertible<T,int>::value
      ));
#  else
      BOOST_STATIC_CONSTANT(bool, value = ::boost::is_arithmetic<T>::value);
#  endif

# endif
  };

  template <class T>
  struct is_numeric
    : boost::integral_constant<bool, ::boost::iterators::detail::is_numeric_impl<T>::value>
  {};

#  if defined(BOOST_HAS_LONG_LONG)
  template <>
  struct is_numeric<boost::long_long_type>
    : boost::true_type {};

  template <>
  struct is_numeric<boost::ulong_long_type>
    : boost::true_type {};
#  endif

#  if defined(BOOST_HAS_INT128)
  template <>
  struct is_numeric<boost::int128_type>
    : boost::true_type {};

  template <>
  struct is_numeric<boost::uint128_type>
    : boost::true_type {};
#  endif

  // Some compilers fail to have a numeric_limits specialization
  template <>
  struct is_numeric<wchar_t>
    : true_type {};

  template <class T>
  struct numeric_difference
  {
      typedef typename boost::detail::numeric_traits<T>::difference_type type;
  };

#  if defined(BOOST_HAS_INT128)
  // std::numeric_limits, which is used by numeric_traits, is not specialized for __int128 in some standard libraries
  template <>
  struct numeric_difference<boost::int128_type>
  {
      typedef boost::int128_type type;
  };

  template <>
  struct numeric_difference<boost::uint128_type>
  {
      typedef boost::int128_type type;
  };
#  endif

  template <class Incrementable, class CategoryOrTraversal, class Difference>
  struct counting_iterator_base
  {
      typedef typename detail::ia_dflt_help<
          CategoryOrTraversal
        , typename boost::conditional<
              is_numeric<Incrementable>::value
            , boost::type_identity<random_access_traversal_tag>
            , iterator_traversal<Incrementable>
          >::type
      >::type traversal;

      typedef typename detail::ia_dflt_help<
          Difference
        , typename boost::conditional<
              is_numeric<Incrementable>::value
            , numeric_difference<Incrementable>
            , iterator_difference<Incrementable>
          >::type
      >::type difference;

      typedef iterator_adaptor<
          counting_iterator<Incrementable, CategoryOrTraversal, Difference> // self
        , Incrementable                                           // Base
        , Incrementable                                           // Value
# ifndef BOOST_ITERATOR_REF_CONSTNESS_KILLS_WRITABILITY
          const  // MSVC won't strip this.  Instead we enable Thomas'
                 // criterion (see boost/iterator/detail/facade_iterator_category.hpp)
# endif
        , traversal
        , Incrementable const&                                    // reference
        , difference
      > type;
  };

  // Template class distance_policy_select -- choose a policy for computing the
  // distance between counting_iterators at compile-time based on whether or not
  // the iterator wraps an integer or an iterator, using "poor man's partial
  // specialization".

  template <bool is_integer> struct distance_policy_select;

  // A policy for wrapped iterators
  template <class Difference, class Incrementable1, class Incrementable2>
  struct iterator_distance
  {
      static Difference distance(Incrementable1 x, Incrementable2 y)
      {
          return y - x;
      }
  };

  // A policy for wrapped numbers
  template <class Difference, class Incrementable1, class Incrementable2>
  struct number_distance
  {
      static Difference distance(Incrementable1 x, Incrementable2 y)
      {
          return boost::detail::numeric_distance(x, y);
      }
  };
}

template <
    class Incrementable
  , class CategoryOrTraversal = use_default
  , class Difference = use_default
>
class counting_iterator
  : public detail::counting_iterator_base<
        Incrementable, CategoryOrTraversal, Difference
    >::type
{
    typedef typename detail::counting_iterator_base<
        Incrementable, CategoryOrTraversal, Difference
    >::type super_t;

    friend class iterator_core_access;

 public:
    typedef typename super_t::difference_type difference_type;

    BOOST_DEFAULTED_FUNCTION(counting_iterator(), {})

    BOOST_DEFAULTED_FUNCTION(counting_iterator(counting_iterator const& rhs), : super_t(rhs.base()) {})

    counting_iterator(Incrementable x)
      : super_t(x)
    {
    }

# if 0
    template<class OtherIncrementable>
    counting_iterator(
        counting_iterator<OtherIncrementable, CategoryOrTraversal, Difference> const& t
      , typename enable_if_convertible<OtherIncrementable, Incrementable>::type* = 0
    )
      : super_t(t.base())
    {}
# endif

    BOOST_DEFAULTED_FUNCTION(counting_iterator& operator=(counting_iterator const& rhs), { *static_cast< super_t* >(this) = static_cast< super_t const& >(rhs); return *this; })

 private:

    typename super_t::reference dereference() const
    {
        return this->base_reference();
    }

    template <class OtherIncrementable>
    difference_type
    distance_to(counting_iterator<OtherIncrementable, CategoryOrTraversal, Difference> const& y) const
    {
        typedef typename boost::conditional<
            detail::is_numeric<Incrementable>::value
          , detail::number_distance<difference_type, Incrementable, OtherIncrementable>
          , detail::iterator_distance<difference_type, Incrementable, OtherIncrementable>
        >::type d;

        return d::distance(this->base(), y.base());
    }
};

// Manufacture a counting iterator for an arbitrary incrementable type
template <class Incrementable>
inline counting_iterator<Incrementable>
make_counting_iterator(Incrementable x)
{
    typedef counting_iterator<Incrementable> result_t;
    return result_t(x);
}

} // namespace iterators

using iterators::counting_iterator;
using iterators::make_counting_iterator;

} // namespace boost

#endif // COUNTING_ITERATOR_DWA200348_HPP
