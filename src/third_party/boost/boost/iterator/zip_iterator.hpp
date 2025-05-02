// Copyright David Abrahams and Thomas Becker 2000-2006.
// Copyright Kohei Takahashi 2012-2014.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_ZIP_ITERATOR_TMB_07_13_2003_HPP_
#define BOOST_ZIP_ITERATOR_TMB_07_13_2003_HPP_

#include <utility> // for std::pair

#include <boost/iterator/iterator_traits.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/enable_if_convertible.hpp>
#include <boost/iterator/iterator_categories.hpp>
#include <boost/iterator/min_category.hpp>

#include <boost/mp11/list.hpp>
#include <boost/mp11/utility.hpp>
#include <boost/fusion/adapted/boost_tuple.hpp> // for backward compatibility
#include <boost/fusion/algorithm/iteration/for_each.hpp>
#include <boost/fusion/algorithm/transformation/transform.hpp>
#include <boost/fusion/sequence/convert.hpp>
#include <boost/fusion/sequence/intrinsic/at_c.hpp>
#include <boost/fusion/sequence/comparison/equal_to.hpp>
#include <boost/fusion/support/tag_of_fwd.hpp>

namespace boost {

// Forward declarations for Boost.Tuple support
namespace tuples {
struct null_type;
template< class, class >
struct cons;
} // namespace tuples

// Forward declarations for Boost.Fusion support
namespace fusion {
struct void_;
} // namespace fusion

namespace iterators {

// Zip iterator forward declaration for zip_iterator_base
template< typename IteratorTuple >
class zip_iterator;

namespace detail {

// Functors to be used with tuple algorithms
//
template< typename DiffType >
class advance_iterator
{
public:
    advance_iterator(DiffType step) :
        m_step(step)
    {}

    template< typename Iterator >
    void operator()(Iterator& it) const { it += m_step; }

private:
    DiffType m_step;
};

struct increment_iterator
{
    template< typename Iterator >
    void operator()(Iterator& it) const { ++it; }
};

struct decrement_iterator
{
    template< typename Iterator >
    void operator()(Iterator& it) const { --it; }
};

struct dereference_iterator
{
    template< typename >
    struct result;

    template< typename This, typename Iterator >
    struct result< This(Iterator) >
    {
        using type = iterator_reference_t<
            typename std::remove_cv< typename std::remove_reference< Iterator >::type >::type
        >;
    };

    template< typename Iterator >
    typename result< dereference_iterator(Iterator) >::type operator()(Iterator const& it) const
    {
        return *it;
    }
};

// The trait checks if the type is a trailing "null" type used to indicate unused template parameters in non-variadic types
template< typename T >
struct is_trailing_null_type : std::false_type {};
template< typename T >
struct is_trailing_null_type< const T > : is_trailing_null_type< T > {};
template< >
struct is_trailing_null_type< tuples::null_type > : std::true_type {};
template< >
struct is_trailing_null_type< fusion::void_ > : std::true_type {};

// Metafunction to obtain the type of the tuple whose element types
// are the reference types of an iterator tuple.
template< typename IteratorTuple >
struct tuple_of_references;

template< typename IteratorTuple >
using tuple_of_references_t = typename tuple_of_references< IteratorTuple >::type;

template< template< typename... > class Tuple, typename... Iterators >
struct tuple_of_references< Tuple< Iterators... > >
{
    // Note: non-variadic Boost.Tuple and Boost.Fusion need special handling
    // to avoid instantiating iterator traits on the trailing "null" types.
    // If not that, we could simply do
    // mp11::mp_transform< iterator_reference_t, IteratorTuple >.
    using type = Tuple<
        mp11::mp_eval_if<
            detail::is_trailing_null_type< Iterators >,
            Iterators,
            iterator_reference_t, Iterators
        >...
    >;
};

template< typename Front, typename Tail >
struct tuple_of_references< tuples::cons< Front, Tail > >
{
    using type = tuples::cons<
        iterator_reference_t< Front >,
        mp11::mp_eval_if<
            detail::is_trailing_null_type< Tail >,
            Tail,
            detail::tuple_of_references_t, Tail
        >
    >;
};

// Metafunction to obtain the minimal traversal tag in a list
// of iterators.
template< typename IteratorList >
struct minimum_traversal_category_in_iterator_list;

template< typename IteratorList >
using minimum_traversal_category_in_iterator_list_t = typename minimum_traversal_category_in_iterator_list< IteratorList >::type;

template< template< typename... > class List, typename... Iterators >
struct minimum_traversal_category_in_iterator_list< List< Iterators... > >
{
    // Note: non-variadic Boost.Tuple and Boost.Fusion need special handling
    // to avoid instantiating iterator traits on the trailing "null" types.
    // For such types just use random_access_traversal_tag, which will not
    // affect the result of min_category.
    using type = min_category_t<
        mp11::mp_eval_if<
            detail::is_trailing_null_type< Iterators >,
            random_access_traversal_tag,
            pure_iterator_traversal_t, Iterators
        >...
    >;
};

template< typename FrontTraversal, typename Tail >
using minimum_traversal_category_in_tail_t = min_category_t<
    FrontTraversal,
    minimum_traversal_category_in_iterator_list_t< Tail >
>;

template< typename Front, typename Tail >
struct minimum_traversal_category_in_iterator_list< tuples::cons< Front, Tail > >
{
    using front_traversal = pure_iterator_traversal_t< Front >;

    using type = mp11::mp_eval_if<
        detail::is_trailing_null_type< Tail >,
        front_traversal,
        minimum_traversal_category_in_tail_t,
            front_traversal,
            Tail
    >;
};

///////////////////////////////////////////////////////////////////
//
// Class zip_iterator_base
//
// Builds and exposes the iterator facade type from which the zip
// iterator will be derived.
//
template< typename IteratorTuple >
struct zip_iterator_base
{
private:
    // Reference type is the type of the tuple obtained from the
    // iterators' reference types.
    using reference = detail::tuple_of_references_t< IteratorTuple >;

    // Value type is the same as reference type.
    using value_type = reference;

    // Difference type is the first iterator's difference type
    using difference_type = iterator_difference_t< mp11::mp_front< IteratorTuple > >;

    // Traversal catetgory is the minimum traversal category in the
    // iterator tuple.
    using traversal_category = detail::minimum_traversal_category_in_iterator_list_t< IteratorTuple >;

public:
    // The iterator facade type from which the zip iterator will
    // be derived.
    using type = iterator_facade<
        zip_iterator< IteratorTuple >,
        value_type,
        traversal_category,
        reference,
        difference_type
    >;
};

template< typename Reference >
struct converter
{
    template< typename Seq >
    static Reference call(Seq seq)
    {
        using tag = typename fusion::traits::tag_of< Reference >::type;
        return fusion::convert< tag >(seq);
    }
};

template< typename Reference1, typename Reference2 >
struct converter< std::pair< Reference1, Reference2 > >
{
    using reference = std::pair< Reference1, Reference2 >;

    template< typename Seq >
    static reference call(Seq seq)
    {
        return reference(fusion::at_c< 0 >(seq), fusion::at_c< 1 >(seq));
    }
};

} // namespace detail

/////////////////////////////////////////////////////////////////////
//
// zip_iterator class definition
//
template< typename IteratorTuple >
class zip_iterator :
    public detail::zip_iterator_base< IteratorTuple >::type
{
    // Typedef super_t as our base class.
    using super_t = typename detail::zip_iterator_base< IteratorTuple >::type;

    // iterator_core_access is the iterator's best friend.
    friend class iterator_core_access;

public:
    // Construction
    // ============

    // Default constructor
    zip_iterator() = default;

    // Constructor from iterator tuple
    zip_iterator(IteratorTuple iterator_tuple) :
        m_iterator_tuple(iterator_tuple)
    {}

    // Copy constructor
    template< typename OtherIteratorTuple, typename = enable_if_convertible_t< OtherIteratorTuple, IteratorTuple > >
    zip_iterator(zip_iterator< OtherIteratorTuple > const& other) :
        m_iterator_tuple(other.get_iterator_tuple())
    {}

    // Get method for the iterator tuple.
    IteratorTuple const& get_iterator_tuple() const { return m_iterator_tuple; }

private:
    // Implementation of Iterator Operations
    // =====================================

    // Dereferencing returns a tuple built from the dereferenced
    // iterators in the iterator tuple.
    typename super_t::reference dereference() const
    {
        using reference = typename super_t::reference;
        using gen = detail::converter< reference >;
        return gen::call(fusion::transform(get_iterator_tuple(), detail::dereference_iterator()));
    }

    // Two zip iterators are equal if all iterators in the iterator
    // tuple are equal. NOTE: It should be possible to implement this
    // as
    //
    // return get_iterator_tuple() == other.get_iterator_tuple();
    //
    // but equality of tuples currently (7/2003) does not compile
    // under several compilers. No point in bringing in a bunch
    // of #ifdefs here.
    //
    template< typename OtherIteratorTuple >
    bool equal(zip_iterator< OtherIteratorTuple > const& other) const
    {
        return fusion::equal_to(get_iterator_tuple(), other.get_iterator_tuple());
    }

    // Advancing a zip iterator means to advance all iterators in the
    // iterator tuple.
    void advance(typename super_t::difference_type n)
    {
        fusion::for_each(m_iterator_tuple, detail::advance_iterator< typename super_t::difference_type >(n));
    }

    // Incrementing a zip iterator means to increment all iterators in
    // the iterator tuple.
    void increment()
    {
        fusion::for_each(m_iterator_tuple, detail::increment_iterator());
    }

    // Decrementing a zip iterator means to decrement all iterators in
    // the iterator tuple.
    void decrement()
    {
        fusion::for_each(m_iterator_tuple, detail::decrement_iterator());
    }

    // Distance is calculated using the first iterator in the tuple.
    template< typename OtherIteratorTuple >
    typename super_t::difference_type distance_to(zip_iterator< OtherIteratorTuple > const& other) const
    {
        return fusion::at_c< 0 >(other.get_iterator_tuple()) - fusion::at_c< 0 >(this->get_iterator_tuple());
    }

private:
    // Data Members
    // ============

    // The iterator tuple.
    IteratorTuple m_iterator_tuple;
};

// Make function for zip iterator
//
template< typename IteratorTuple >
inline zip_iterator< IteratorTuple > make_zip_iterator(IteratorTuple t)
{
    return zip_iterator< IteratorTuple >(t);
}

} // namespace iterators

using iterators::zip_iterator;
using iterators::make_zip_iterator;

} // namespace boost

#endif
