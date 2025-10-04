// (C) Copyright David Abrahams 2002.
// (C) Copyright Jeremy Siek    2002.
// (C) Copyright Thomas Witt    2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_FILTER_ITERATOR_23022003THW_HPP
#define BOOST_FILTER_ITERATOR_23022003THW_HPP

#include <type_traits>

#include <boost/core/use_default.hpp>
#include <boost/core/empty_value.hpp>
#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/iterator/iterator_categories.hpp>
#include <boost/iterator/enable_if_convertible.hpp>

namespace boost {
namespace iterators {

template< typename Predicate, typename Iterator >
class filter_iterator;

namespace detail {

template< typename Predicate, typename Iterator >
using filter_iterator_base_t = iterator_adaptor<
    filter_iterator< Predicate, Iterator >,
    Iterator,
    use_default,
    typename std::conditional<
        std::is_convertible<
            iterator_traversal_t< Iterator >,
            random_access_traversal_tag
        >::value,
        bidirectional_traversal_tag,
        use_default
    >::type
>;

} // namespace detail

template< typename Predicate, typename Iterator >
class filter_iterator :
    public detail::filter_iterator_base_t< Predicate, Iterator >
{
    friend class iterator_core_access;

    template< typename, typename >
    friend class filter_iterator;

private:
    using super_t = detail::filter_iterator_base_t< Predicate, Iterator >;

    // Storage class to leverage EBO, when possible
    struct storage :
        private boost::empty_value< Predicate >
    {
        using predicate_base = boost::empty_value< Predicate >;

        Iterator m_end;

        storage() = default;

        template<
            typename Iter,
            typename = typename std::enable_if<
                !std::is_same<
                    typename std::remove_cv< typename std::remove_reference< Iter >::type >::type,
                    storage
                >::value
            >
        >
        explicit storage(Iter&& end) :
            predicate_base(boost::empty_init_t{}), m_end(static_cast< Iterator&& >(end))
        {
        }

        template< typename Pred, typename Iter >
        storage(Pred&& pred, Iter&& end) :
            predicate_base(boost::empty_init_t{}, static_cast< Pred&& >(pred)), m_end(static_cast< Iter&& >(end))
        {
        }

        Predicate& predicate() noexcept { return predicate_base::get(); }
        Predicate const& predicate() const noexcept { return predicate_base::get(); }
    };

public:
    filter_iterator() = default;

    filter_iterator(Predicate f, Iterator x, Iterator end = Iterator()) :
        super_t(static_cast< Iterator&& >(x)), m_storage(static_cast< Predicate&& >(f), static_cast< Iterator&& >(end))
    {
        satisfy_predicate();
    }

    template< bool Requires = std::is_class< Predicate >::value, typename = typename std::enable_if< Requires >::type >
    filter_iterator(Iterator x, Iterator end = Iterator()) :
        super_t(static_cast< Iterator&& >(x)), m_storage(static_cast< Iterator&& >(end))
    {
        satisfy_predicate();
    }

    template< typename OtherIterator, typename = enable_if_convertible_t< OtherIterator, Iterator > >
    filter_iterator(filter_iterator< Predicate, OtherIterator > const& t) :
        super_t(t.base()), m_storage(t.m_storage.predicate(), m_storage.m_end)
    {}

    Predicate predicate() const { return m_storage.predicate(); }
    Iterator end() const { return m_storage.m_end; }

private:
    void increment()
    {
        ++(this->base_reference());
        satisfy_predicate();
    }

    void decrement()
    {
        while (!m_storage.predicate()(*--(this->base_reference()))) {}
    }

    void satisfy_predicate()
    {
        while (this->base() != m_storage.m_end && !m_storage.predicate()(*this->base()))
            ++(this->base_reference());
    }

private:
    storage m_storage;
};

template< typename Predicate, typename Iterator >
inline filter_iterator< Predicate, Iterator > make_filter_iterator(Predicate f, Iterator x, Iterator end = Iterator())
{
    return filter_iterator< Predicate, Iterator >(static_cast< Predicate&& >(f), static_cast< Iterator&& >(x), static_cast< Iterator&& >(end));
}

template< typename Predicate, typename Iterator >
inline typename std::enable_if<
    std::is_class< Predicate >::value,
    filter_iterator< Predicate, Iterator >
>::type make_filter_iterator(Iterator x, Iterator end = Iterator())
{
    return filter_iterator< Predicate, Iterator >(static_cast< Iterator&& >(x), static_cast< Iterator&& >(end));
}

} // namespace iterators

using iterators::filter_iterator;
using iterators::make_filter_iterator;

} // namespace boost

#endif // BOOST_FILTER_ITERATOR_23022003THW_HPP
