// (C) Copyright Ronald Garcia 2002. Permission to copy, use, modify, sell and
// distribute this software is granted provided this copyright notice appears
// in all copies. This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.

// See http://www.boost.org/libs/utility/shared_container_iterator.html for documentation.

#ifndef BOOST_ITERATOR_SHARED_CONTAINER_ITERATOR_HPP_INCLUDED_
#define BOOST_ITERATOR_SHARED_CONTAINER_ITERATOR_HPP_INCLUDED_

#include <memory>
#include <utility>
#include <boost/iterator/iterator_adaptor.hpp>

namespace boost {

// For backward compatibility with boost::shared_ptr
template< class T >
class shared_ptr;

namespace iterators {
namespace detail {

// Fake deleter that holds an instance of boost::shared_ptr and through it keeps the pointed object from deletion
template< typename T >
class shared_container_iterator_bsptr_holder
{
private:
    boost::shared_ptr< T > m_ptr;

public:
    explicit shared_container_iterator_bsptr_holder(boost::shared_ptr< T > const& ptr) :
        m_ptr(ptr)
    {}

    void operator()(T*) const noexcept {}
};

} // namespace detail

template< typename Container >
class shared_container_iterator :
    public iterator_adaptor<
        shared_container_iterator< Container >,
        typename Container::iterator
    >
{
private:
    using super_t = iterator_adaptor<
        shared_container_iterator< Container >,
        typename Container::iterator
    >;

    using iterator_t = typename Container::iterator;
    using container_ref_t = std::shared_ptr< Container >;

public:
    shared_container_iterator() = default;

    shared_container_iterator(iterator_t const& x, container_ref_t const& c) :
        super_t(x),
        m_container_ref(c)
    {}

    // Constructor for backward compatibility with boost::shared_ptr
    shared_container_iterator(iterator_t const& x, boost::shared_ptr< Container > const& c) :
        super_t(x),
        m_container_ref(c.get(), detail::shared_container_iterator_bsptr_holder< Container >(c))
    {}

private:
    container_ref_t m_container_ref;
};

template< typename Container >
inline shared_container_iterator< Container >
make_shared_container_iterator(typename Container::iterator iter, std::shared_ptr< Container > const& container)
{
    return shared_container_iterator< Container >(iter, container);
}

template< typename Container >
inline std::pair< shared_container_iterator< Container >, shared_container_iterator< Container > >
make_shared_container_range(std::shared_ptr< Container > const& container)
{
    return std::make_pair
    (
        iterators::make_shared_container_iterator(container->begin(), container),
        iterators::make_shared_container_iterator(container->end(), container)
    );
}

// Factory functions for backward compatibility with boost::shared_ptr
template< typename Container >
inline shared_container_iterator< Container >
make_shared_container_iterator(typename Container::iterator iter, boost::shared_ptr< Container > const& container)
{
    return shared_container_iterator< Container >(iter, container);
}

template< typename Container >
inline std::pair< shared_container_iterator< Container >, shared_container_iterator< Container > >
make_shared_container_range(boost::shared_ptr< Container > const& container)
{
    std::shared_ptr< Container > c(container.get(), detail::shared_container_iterator_bsptr_holder< Container >(container));
    return iterators::make_shared_container_range(std::move(c));
}

} // namespace iterators

using iterators::shared_container_iterator;
using iterators::make_shared_container_iterator;
using iterators::make_shared_container_range;

} // namespace boost

#endif // BOOST_ITERATOR_SHARED_CONTAINER_ITERATOR_HPP_INCLUDED_
