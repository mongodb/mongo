// Copyright 2009 (C) Dean Michael Berris <me@deanberris.com>
// Copyright 2012 (C) Google, Inc.
// Copyright 2012 (C) Jeffrey Lee Hellrung, Jr.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ITERATOR_FUNCTION_INPUT_ITERATOR_HPP_INCLUDED_
#define BOOST_ITERATOR_FUNCTION_INPUT_ITERATOR_HPP_INCLUDED_

#include <memory>
#include <type_traits>

#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/iterator_categories.hpp>
#include <boost/iterator/detail/type_traits/conjunction.hpp>
#include <boost/optional/optional.hpp>

namespace boost {
namespace iterators {

template< typename Function, typename Input >
class function_input_iterator;

namespace detail {

template< typename Function, typename Input >
using function_input_iterator_facade_base_t = iterator_facade<
    iterators::function_input_iterator< Function, Input >,
    decltype(std::declval< Function& >()()),
    single_pass_traversal_tag,
    decltype(std::declval< Function& >()()) const&
>;

template< typename Function, typename Input >
class function_object_input_iterator :
    public function_input_iterator_facade_base_t< Function, Input >
{
private:
    using base_type = function_input_iterator_facade_base_t< Function, Input >;

protected:
    using function_arg_type = Function&;

public:
    using value_type = typename base_type::value_type;

public:
    function_object_input_iterator(function_arg_type f, Input state) :
        m_f(std::addressof(f)), m_state(state)
    {}

protected:
    typename std::add_pointer< Function >::type m_f;
    Input m_state;
    mutable optional< value_type > m_value;
};

template< typename Function, typename Input >
class function_pointer_input_iterator :
    public function_input_iterator_facade_base_t< Function, Input >
{
private:
    using base_type = function_input_iterator_facade_base_t< Function, Input >;

protected:
    using function_arg_type = Function;

public:
    using value_type = typename base_type::value_type;

public:
    function_pointer_input_iterator(function_arg_type f, Input state) :
        m_f(f), m_state(state)
    {}

protected:
    Function m_f;
    Input m_state;
    mutable optional< value_type > m_value;
};

template< typename Function, typename Input >
using function_input_iterator_base_t = typename std::conditional<
    detail::conjunction<
        std::is_pointer< Function >,
        std::is_function< typename std::remove_pointer< Function >::type >
    >::value,
    detail::function_pointer_input_iterator< Function, Input >,
    detail::function_object_input_iterator< Function, Input >
>::type;

} // namespace detail

template< typename Function, typename Input >
class function_input_iterator :
    public detail::function_input_iterator_base_t< Function, Input >
{
private:
    using base_type = detail::function_input_iterator_base_t< Function, Input >;
    using function_arg_type = typename base_type::function_arg_type;

public:
    using reference = typename base_type::reference;

public:
    function_input_iterator(function_arg_type f, Input i) :
        base_type(f, i)
    {}

    void increment()
    {
        if (this->m_value)
            this->m_value.reset();
        else
            (*this->m_f)();
        ++this->m_state;
    }

    reference dereference() const
    {
        if (!this->m_value)
            this->m_value = (*this->m_f)();
        return this->m_value.get();
    }

    bool equal(function_input_iterator const& other) const
    {
        return this->m_f == other.m_f && this->m_state == other.m_state;
    }
};

template< typename Function, typename Input >
inline function_input_iterator< Function, Input > make_function_input_iterator(Function& f, Input state)
{
    return function_input_iterator< Function, Input >(f, state);
}

template< typename Function, typename Input >
inline function_input_iterator< Function*, Input > make_function_input_iterator(Function* f, Input state)
{
    return function_input_iterator< Function*, Input >(f, state);
}

struct infinite
{
    infinite& operator++() { return *this; }
    infinite& operator++(int) { return *this; }
    bool operator==(infinite&) const { return false; };
    bool operator==(infinite const&) const { return false; };
};

} // namespace iterators

using iterators::function_input_iterator;
using iterators::make_function_input_iterator;
using iterators::infinite;

} // namespace boost

#endif // BOOST_ITERATOR_FUNCTION_INPUT_ITERATOR_HPP_INCLUDED_
