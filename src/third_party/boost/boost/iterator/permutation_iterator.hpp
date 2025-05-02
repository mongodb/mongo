// (C) Copyright Toon Knapen    2001.
// (C) Copyright David Abrahams 2003.
// (C) Copyright Roland Richter 2003.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_ITERATOR_PERMUTATION_ITERATOR_HPP_INCLUDED_
#define BOOST_ITERATOR_PERMUTATION_ITERATOR_HPP_INCLUDED_

#include <iterator>

#include <boost/core/use_default.hpp>
#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/iterator/enable_if_convertible.hpp>

namespace boost {
namespace iterators {

template< typename ElementIterator, typename IndexIterator >
class permutation_iterator :
    public iterator_adaptor<
        permutation_iterator< ElementIterator, IndexIterator >,
        IndexIterator,
        typename std::iterator_traits< ElementIterator >::value_type,
        use_default,
        typename std::iterator_traits< ElementIterator >::reference
    >
{
    friend class iterator_core_access;
    template< typename, typename >
    friend class permutation_iterator;

private:
    using super_t = iterator_adaptor<
        permutation_iterator< ElementIterator, IndexIterator >,
        IndexIterator,
        typename std::iterator_traits< ElementIterator >::value_type,
        use_default,
        typename std::iterator_traits< ElementIterator >::reference
    >;

public:
    permutation_iterator() :
        m_elt_iter()
    {}

    explicit permutation_iterator(ElementIterator x, IndexIterator y) :
        super_t(y),
        m_elt_iter(x)
    {}

    template<
        typename OtherElementIterator,
        typename OtherIndexIterator,
        typename = enable_if_convertible_t< OtherElementIterator, ElementIterator >,
        typename = enable_if_convertible_t< OtherIndexIterator, IndexIterator >
    >
    permutation_iterator(permutation_iterator< OtherElementIterator, OtherIndexIterator > const& r) :
        super_t(r.base()),
        m_elt_iter(r.m_elt_iter)
    {}

private:
    typename super_t::reference dereference() const { return *(m_elt_iter + *this->base()); }

private:
    ElementIterator m_elt_iter;
};


template< typename ElementIterator, typename IndexIterator >
inline permutation_iterator< ElementIterator, IndexIterator > make_permutation_iterator(ElementIterator e, IndexIterator i)
{
    return permutation_iterator< ElementIterator, IndexIterator >(e, i);
}

} // namespace iterators

using iterators::permutation_iterator;
using iterators::make_permutation_iterator;

} // namespace boost

#endif // BOOST_ITERATOR_PERMUTATION_ITERATOR_HPP_INCLUDED_
