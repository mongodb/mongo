// (C) Copyright Jens Maurer 2001.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// Revision History:

// 15 Nov 2001   Jens Maurer
//      created.

//  See http://www.boost.org/libs/utility/iterator_adaptors.htm for documentation.

#ifndef BOOST_ITERATOR_GENERATOR_ITERATOR_HPP_INCLUDED_
#define BOOST_ITERATOR_GENERATOR_ITERATOR_HPP_INCLUDED_

#include <memory>
#include <type_traits>

#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/iterator_categories.hpp>

namespace boost {
namespace iterators {

template< typename Generator >
class generator_iterator :
    public iterator_facade<
        generator_iterator< Generator >,
        decltype(std::declval< Generator& >()()),
        single_pass_traversal_tag,
        decltype(std::declval< Generator& >()()) const&
    >
{
    friend class iterator_core_access;

private:
    using super_t = iterator_facade<
        generator_iterator< Generator >,
        decltype(std::declval< Generator& >()()),
        single_pass_traversal_tag,
        decltype(std::declval< Generator& >()()) const&
    >;

public:
    generator_iterator() :
        m_g(nullptr),
        m_value()
    {}

    generator_iterator(Generator* g) :
        m_g(g),
        m_value((*m_g)())
    {}

private:
    void increment()
    {
        m_value = (*m_g)();
    }

    typename super_t::reference dereference() const
    {
        return m_value;
    }

    bool equal(generator_iterator const& y) const
    {
        return m_g == y.m_g && m_value == y.m_value;
    }

private:
    Generator* m_g;
    typename Generator::result_type m_value;
};

template< typename Generator >
struct generator_iterator_generator
{
    using type = generator_iterator< Generator >;
};

template< typename Generator >
inline generator_iterator< Generator > make_generator_iterator(Generator& gen)
{
    return generator_iterator< Generator >(std::addressof(gen));
}

} // namespace iterators

using iterators::generator_iterator;
using iterators::generator_iterator_generator;
using iterators::make_generator_iterator;

} // namespace boost

#endif // BOOST_ITERATOR_GENERATOR_ITERATOR_HPP_INCLUDED_
