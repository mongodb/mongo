// (C) Copyright Jeremy Siek 2001.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// Revision History:

// 27 Feb 2001   Jeremy Siek
//      Initial checkin.

#ifndef BOOST_ITERATOR_FUNCTION_OUTPUT_ITERATOR_HPP_INCLUDED_
#define BOOST_ITERATOR_FUNCTION_OUTPUT_ITERATOR_HPP_INCLUDED_

#include <cstddef>
#include <iterator>
#include <type_traits>

namespace boost {
namespace iterators {

template< typename UnaryFunction >
class function_output_iterator
{
private:
    class output_proxy
    {
    public:
        explicit output_proxy(UnaryFunction& f) noexcept :
            m_f(f)
        {}

        template< typename T >
        typename std::enable_if<
            !std::is_same< typename std::remove_cv< typename std::remove_reference< T >::type >::type, output_proxy >::value,
            output_proxy const&
        >::type operator=(T&& value) const
        {
            m_f(static_cast< T&& >(value));
            return *this;
        }

        output_proxy(output_proxy const& that) = default;
        output_proxy& operator=(output_proxy const&) = delete;

    private:
        UnaryFunction& m_f;
    };

public:
    using iterator_category = std::output_iterator_tag;
    using value_type = void;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = void;

    template<
        bool Requires = std::is_class< UnaryFunction >::value,
        typename = typename std::enable_if< Requires >::type
    >
    function_output_iterator() :
        m_f()
    {}

    explicit function_output_iterator(UnaryFunction const& f) :
        m_f(f)
    {}

    output_proxy operator*() { return output_proxy(m_f); }
    function_output_iterator& operator++() { return *this; }
    function_output_iterator& operator++(int) { return *this; }

private:
    UnaryFunction m_f;
};

template< typename UnaryFunction >
inline function_output_iterator< UnaryFunction > make_function_output_iterator(UnaryFunction const& f = UnaryFunction())
{
    return function_output_iterator< UnaryFunction >(f);
}

} // namespace iterators

using iterators::function_output_iterator;
using iterators::make_function_output_iterator;

} // namespace boost

#endif // BOOST_ITERATOR_FUNCTION_OUTPUT_ITERATOR_HPP_INCLUDED_
