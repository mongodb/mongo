// (C) Copyright David Abrahams 2002.
// (C) Copyright Jeremy Siek    2002.
// (C) Copyright Thomas Witt    2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_ITERATOR_REVERSE_ITERATOR_23022003THW_HPP
#define BOOST_ITERATOR_REVERSE_ITERATOR_23022003THW_HPP

#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/iterator/enable_if_convertible.hpp>

namespace boost {
namespace iterators {

template< typename Iterator >
class reverse_iterator :
    public iterator_adaptor< reverse_iterator< Iterator >, Iterator >
{
    friend class iterator_core_access;

private:
    using super_t = iterator_adaptor< reverse_iterator< Iterator >, Iterator >;

public:
    reverse_iterator() = default;

    explicit reverse_iterator(Iterator x) :
        super_t(x)
    {}

    template<
        typename OtherIterator,
        typename = enable_if_convertible_t< OtherIterator, Iterator >
    >
    reverse_iterator(reverse_iterator< OtherIterator > const& r) :
        super_t(r.base())
    {}

private:
    typename super_t::reference dereference() const
    {
        Iterator it = this->base_reference();
        --it;
        return *it;
    }

    void increment() { --this->base_reference(); }
    void decrement() { ++this->base_reference(); }

    void advance(typename super_t::difference_type n)
    {
        this->base_reference() -= n;
    }

    template< typename OtherIterator >
    typename super_t::difference_type distance_to(reverse_iterator< OtherIterator > const& y) const
    {
        return this->base_reference() - y.base();
    }
};

template< typename Iterator >
inline reverse_iterator< Iterator > make_reverse_iterator(Iterator x)
{
    return reverse_iterator< Iterator >(x);
}

} // namespace iterators

using iterators::reverse_iterator;
using iterators::make_reverse_iterator;

} // namespace boost

#endif // BOOST_ITERATOR_REVERSE_ITERATOR_23022003THW_HPP
