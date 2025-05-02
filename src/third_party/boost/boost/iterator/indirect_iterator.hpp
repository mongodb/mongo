// (C) Copyright David Abrahams 2002.
// (C) Copyright Jeremy Siek    2002.
// (C) Copyright Thomas Witt    2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_INDIRECT_ITERATOR_23022003THW_HPP
#define BOOST_INDIRECT_ITERATOR_23022003THW_HPP

#include <iterator>
#include <type_traits>

#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/iterator/enable_if_convertible.hpp>
#include <boost/iterator/detail/eval_if_default.hpp>

#include <boost/pointee.hpp>
#include <boost/indirect_reference.hpp>

#include <boost/core/use_default.hpp>

namespace boost {
namespace iterators {

template< typename Iter, typename Value, typename Category, typename Reference, typename Difference >
class indirect_iterator;

namespace detail {

template< typename Iter, typename Value, typename Category, typename Reference, typename Difference >
struct indirect_base
{
    using dereferenceable = typename std::iterator_traits< Iter >::value_type;

    using type = iterator_adaptor<
        indirect_iterator< Iter, Value, Category, Reference, Difference >,
        Iter,
        detail::eval_if_default_t<
            Value,
            pointee< dereferenceable >
        >,
        Category,
        detail::eval_if_default_t<
            Reference,
            detail::eval_if_default<
                Value,
                indirect_reference< dereferenceable >,
                std::add_lvalue_reference< Value >
            >
        >,
        Difference
    >;
};

} // namespace detail


template<
    typename Iterator,
    typename Value = use_default,
    typename Category = use_default,
    typename Reference = use_default,
    typename Difference = use_default
>
class indirect_iterator :
    public detail::indirect_base<
        Iterator, Value, Category, Reference, Difference
    >::type
{
    using super_t = typename detail::indirect_base<
        Iterator, Value, Category, Reference, Difference
    >::type;

    friend class iterator_core_access;

public:
    indirect_iterator() = default;

    indirect_iterator(Iterator iter) :
        super_t(iter)
    {}

    template<
        typename Iterator2,
        typename Value2,
        typename Category2,
        typename Reference2,
        typename Difference2,
        typename = enable_if_convertible_t< Iterator2, Iterator >
    >
    indirect_iterator(indirect_iterator< Iterator2, Value2, Category2, Reference2, Difference2 > const& y) :
        super_t(y.base())
    {}

private:
    typename super_t::reference dereference() const
    {
        return **this->base();
    }
};

template< typename Iter >
inline indirect_iterator< Iter > make_indirect_iterator(Iter x)
{
    return indirect_iterator< Iter >(x);
}

template< typename Value, typename Iter >
inline indirect_iterator< Iter, Value > make_indirect_iterator(Iter x)
{
    return indirect_iterator< Iter, Value >(x);
}

} // namespace iterators

using iterators::indirect_iterator;
using iterators::make_indirect_iterator;

} // namespace boost

#endif // BOOST_INDIRECT_ITERATOR_23022003THW_HPP
