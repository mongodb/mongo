// Copyright David Abrahams 2003.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_ITERATOR_COUNTING_ITERATOR_DWA200348_HPP
#define BOOST_ITERATOR_COUNTING_ITERATOR_DWA200348_HPP

#include <limits>
#include <type_traits>

#include <boost/config.hpp>
#include <boost/core/use_default.hpp>
#include <boost/detail/numeric_traits.hpp>
#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/iterator/iterator_categories.hpp>
#include <boost/iterator/detail/if_default.hpp>
#include <boost/iterator/detail/eval_if_default.hpp>
#include <boost/iterator/detail/type_traits/type_identity.hpp>

namespace boost {
namespace iterators {

template<
    typename Incrementable,
    typename CategoryOrTraversal,
    typename Difference
>
class counting_iterator;

namespace detail {

// Try to detect numeric types at compile time in ways compatible
// with the limitations of the compiler and library.
template< typename T >
struct is_numeric :
    public std::integral_constant< bool, std::numeric_limits< T >::is_specialized >
{};

template<>
struct is_numeric< long long > :
    public std::true_type
{};

template<>
struct is_numeric< unsigned long long > :
    public std::true_type
{};

#if defined(BOOST_HAS_INT128)
template<>
struct is_numeric< boost::int128_type > :
    public std::true_type
{};

template<>
struct is_numeric< boost::uint128_type > :
    public std::true_type
{};
#endif

// Some compilers fail to have a numeric_limits specialization
template<>
struct is_numeric< wchar_t > :
    public std::true_type
{};

template< typename T >
struct numeric_difference
{
    using type = typename boost::detail::numeric_traits< T >::difference_type;
};

#if defined(BOOST_HAS_INT128)
// std::numeric_limits, which is used by numeric_traits, is not specialized for __int128 in some standard libraries
template<>
struct numeric_difference< boost::int128_type >
{
    using type = boost::int128_type;
};

template<>
struct numeric_difference< boost::uint128_type >
{
    using type = boost::int128_type;
};
#endif

template< typename Incrementable, typename CategoryOrTraversal, typename Difference, bool IsNumeric = is_numeric< Incrementable >::value >
struct counting_iterator_types
{
    using traversal = detail::eval_if_default_t<
        CategoryOrTraversal,
        iterator_traversal< Incrementable >
    >;

    using difference = detail::eval_if_default_t<
        Difference,
        iterator_difference< Incrementable >
    >;
};

template< typename Incrementable, typename CategoryOrTraversal, typename Difference >
struct counting_iterator_types< Incrementable, CategoryOrTraversal, Difference, true >
{
    using traversal = detail::if_default_t<
        CategoryOrTraversal,
        random_access_traversal_tag
    >;

    using difference = detail::eval_if_default_t<
        Difference,
        numeric_difference< Incrementable >
    >;
};

template< typename Incrementable, typename CategoryOrTraversal, typename Difference >
struct counting_iterator_base
{
    using iterator_types = counting_iterator_types< Incrementable, CategoryOrTraversal, Difference >;

    using type = iterator_adaptor<
        counting_iterator< Incrementable, CategoryOrTraversal, Difference >, // self
        Incrementable,                                           // Base
#ifndef BOOST_ITERATOR_REF_CONSTNESS_KILLS_WRITABILITY
        const  // MSVC won't strip this.  Instead we enable Thomas'
               // criterion (see boost/iterator/detail/facade_iterator_category.hpp)
#endif
        Incrementable,                                           // Value
        typename iterator_types::traversal,
        Incrementable const&,                                    // reference
        typename iterator_types::difference
    >;
};

// A distance calculation policy for wrapped iterators
template< typename Difference, typename Incrementable1, typename Incrementable2 >
struct iterator_distance
{
    static Difference distance(Incrementable1 x, Incrementable2 y)
    {
        return y - x;
    }
};

// A distance calculation policy for wrapped numbers
template< typename Difference, typename Incrementable1, typename Incrementable2 >
struct number_distance
{
    static Difference distance(Incrementable1 x, Incrementable2 y)
    {
        return boost::detail::numeric_distance(x, y);
    }
};

} // namespace detail

template<
    typename Incrementable,
    typename CategoryOrTraversal = use_default,
    typename Difference = use_default
>
class counting_iterator :
    public detail::counting_iterator_base< Incrementable, CategoryOrTraversal, Difference >::type
{
    friend class iterator_core_access;

private:
    using super_t = typename detail::counting_iterator_base<
        Incrementable, CategoryOrTraversal, Difference
    >::type;

public:
    using reference = typename super_t::reference;
    using difference_type = typename super_t::difference_type;

    counting_iterator() = default;

    counting_iterator(counting_iterator const&) = default;
    counting_iterator& operator=(counting_iterator const&) = default;

    counting_iterator(Incrementable x) :
        super_t(x)
    {
    }

private:
    reference dereference() const
    {
        return this->base_reference();
    }

    template< typename OtherIncrementable >
    difference_type
    distance_to(counting_iterator< OtherIncrementable, CategoryOrTraversal, Difference > const& y) const
    {
        using distance_traits = typename std::conditional<
            detail::is_numeric< Incrementable >::value,
            detail::number_distance< difference_type, Incrementable, OtherIncrementable >,
            detail::iterator_distance< difference_type, Incrementable, OtherIncrementable >
        >::type;

        return distance_traits::distance(this->base(), y.base());
    }
};

// Manufacture a counting iterator for an arbitrary incrementable type
template< typename Incrementable >
inline counting_iterator< Incrementable > make_counting_iterator(Incrementable x)
{
    return counting_iterator< Incrementable >(x);
}

} // namespace iterators

using iterators::counting_iterator;
using iterators::make_counting_iterator;

} // namespace boost

#endif // BOOST_ITERATOR_COUNTING_ITERATOR_DWA200348_HPP
