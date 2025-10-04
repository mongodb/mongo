// (C) Copyright David Abrahams 2002.
// (C) Copyright Jeremy Siek    2002.
// (C) Copyright Thomas Witt    2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_ITERATOR_TRANSFORM_ITERATOR_23022003THW_HPP
#define BOOST_ITERATOR_TRANSFORM_ITERATOR_23022003THW_HPP

#include <iterator>
#include <type_traits>

#include <boost/core/use_default.hpp>
#include <boost/core/empty_value.hpp>
#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/iterator/enable_if_convertible.hpp>
#include <boost/iterator/detail/eval_if_default.hpp>

namespace boost {
namespace iterators {

template<
    typename UnaryFunction,
    typename Iterator,
    typename Reference = use_default,
    typename Value = use_default
>
class transform_iterator;

namespace detail {

template< typename UnaryFunc, typename Iterator >
struct transform_iterator_default_reference
{
    using type = decltype(std::declval< UnaryFunc const& >()(std::declval< typename std::iterator_traits< Iterator >::reference >()));
};

// Compute the iterator_adaptor instantiation to be used for transform_iterator
template< typename UnaryFunc, typename Iterator, typename Reference, typename Value >
struct transform_iterator_base
{
private:
    // By default, dereferencing the iterator yields the same as
    // the function.
    using reference = detail::eval_if_default_t<
        Reference,
        transform_iterator_default_reference< UnaryFunc, Iterator >
    >;

    // To get the default for Value: remove any reference on the
    // result type, but retain any constness to signal
    // non-writability.  Note that if we adopt Thomas' suggestion
    // to key non-writability *only* on the Reference argument,
    // we'd need to strip constness here as well.
    using cv_value_type = detail::eval_if_default_t<
        Value,
        std::remove_reference< reference >
    >;

 public:
    using type = iterator_adaptor<
        transform_iterator< UnaryFunc, Iterator, Reference, Value >,
        Iterator,
        cv_value_type,
        use_default,    // Leave the traversal category alone
        reference
    >;
};

} // namespace detail

template< typename UnaryFunc, typename Iterator, typename Reference, typename Value >
class transform_iterator :
    public detail::transform_iterator_base< UnaryFunc, Iterator, Reference, Value >::type,
    private boost::empty_value< UnaryFunc >
{
    friend class iterator_core_access;

private:
    using super_t = typename detail::transform_iterator_base< UnaryFunc, Iterator, Reference, Value >::type;
    using functor_base = boost::empty_value< UnaryFunc >;

public:
    transform_iterator() = default;

    transform_iterator(Iterator const& x, UnaryFunc f) :
        super_t(x),
        functor_base(boost::empty_init_t{}, f)
    {}

    // don't provide this constructor if UnaryFunc is a
    // function pointer type, since it will be 0.  Too dangerous.
    template< bool Requires = std::is_class< UnaryFunc >::value, typename = typename std::enable_if< Requires >::type >
    explicit transform_iterator(Iterator const& x) :
        super_t(x)
    {}

    template<
        typename OtherUnaryFunction,
        typename OtherIterator,
        typename OtherReference,
        typename OtherValue,
        typename = enable_if_convertible_t< OtherIterator, Iterator >,
        typename = enable_if_convertible_t< OtherUnaryFunction, UnaryFunc >
    >
    transform_iterator(transform_iterator< OtherUnaryFunction, OtherIterator, OtherReference, OtherValue > const& t) :
        super_t(t.base()),
        functor_base(boost::empty_init_t{}, t.functor())
    {}

    UnaryFunc functor() const { return functor_base::get(); }

private:
    typename super_t::reference dereference() const { return functor_base::get()(*this->base()); }
};

template< typename UnaryFunc, typename Iterator >
inline transform_iterator< UnaryFunc, Iterator > make_transform_iterator(Iterator it, UnaryFunc fun)
{
    return transform_iterator< UnaryFunc, Iterator >(it, fun);
}

// Version which allows explicit specification of the UnaryFunc
// type.
//
// This generator is not provided if UnaryFunc is a function
// pointer type, because it's too dangerous: the default-constructed
// function pointer in the iterator be 0, leading to a runtime
// crash.
template< typename UnaryFunc, typename Iterator >
inline typename std::enable_if<
    std::is_class< UnaryFunc >::value,   // We should probably find a cheaper test than is_class<>
    transform_iterator< UnaryFunc, Iterator >
>::type make_transform_iterator(Iterator it)
{
    return transform_iterator< UnaryFunc, Iterator >(it);
}

} // namespace iterators

using iterators::transform_iterator;
using iterators::make_transform_iterator;

} // namespace boost

#endif // BOOST_ITERATOR_TRANSFORM_ITERATOR_23022003THW_HPP
