// (C) Copyright David Abrahams 2002.
// (C) Copyright Jeremy Siek    2002.
// (C) Copyright Thomas Witt    2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_ITERATOR_ADAPTOR_23022003THW_HPP
#define BOOST_ITERATOR_ADAPTOR_23022003THW_HPP

#include <type_traits>

#include <boost/core/use_default.hpp>

#include <boost/iterator/iterator_categories.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/iterator_traits.hpp>
#include <boost/iterator/enable_if_convertible.hpp> // for backward compatibility; remove once downstream users are updated
#include <boost/iterator/detail/eval_if_default.hpp>

#include <boost/iterator/detail/config_def.hpp>

namespace boost {
namespace iterators {

// Used as a default template argument internally, merely to
// indicate "use the default", this can also be passed by users
// explicitly in order to specify that the default should be used.
using boost::use_default;

namespace detail {

// A metafunction which computes an iterator_adaptor's base class,
// a specialization of iterator_facade.
template<
    typename Derived,
    typename Base,
    typename Value,
    typename Traversal,
    typename Reference,
    typename Difference
>
using iterator_adaptor_base_t = iterator_facade<
    Derived,

#ifdef BOOST_ITERATOR_REF_CONSTNESS_KILLS_WRITABILITY
    detail::eval_if_default_t<
        Value,
        detail::eval_if_default<
            Reference,
            iterator_value< Base >,
            std::remove_reference< Reference >
        >
    >,
#else
    detail::eval_if_default_t<
        Value,
        iterator_value< Base >
    >,
#endif

    detail::eval_if_default_t<
        Traversal,
        iterator_traversal< Base >
    >,

    detail::eval_if_default_t<
        Reference,
        detail::eval_if_default<
            Value,
            iterator_reference< Base >,
            std::add_lvalue_reference< Value >
        >
    >,

    detail::eval_if_default_t<
        Difference,
        iterator_difference< Base >
    >
>;

} // namespace detail

//
// Iterator Adaptor
//
// The parameter ordering changed slightly with respect to former
// versions of iterator_adaptor The idea is that when the user needs
// to fiddle with the reference type it is highly likely that the
// iterator category has to be adjusted as well.  Any of the
// following four template arguments may be omitted or explicitly
// replaced by use_default.
//
//   Value - if supplied, the value_type of the resulting iterator, unless
//      const. If const, a conforming compiler strips constness for the
//      value_type. If not supplied, iterator_traits<Base>::value_type is used
//
//   Category - the traversal category of the resulting iterator. If not
//      supplied, iterator_traversal<Base>::type is used.
//
//   Reference - the reference type of the resulting iterator, and in
//      particular, the result type of operator*(). If not supplied but
//      Value is supplied, Value& is used. Otherwise
//      iterator_traits<Base>::reference is used.
//
//   Difference - the difference_type of the resulting iterator. If not
//      supplied, iterator_traits<Base>::difference_type is used.
//
template<
    typename Derived,
    typename Base,
    typename Value        = use_default,
    typename Traversal    = use_default,
    typename Reference    = use_default,
    typename Difference   = use_default
>
class iterator_adaptor :
    public detail::iterator_adaptor_base_t<
        Derived, Base, Value, Traversal, Reference, Difference
    >
{
    friend class iterator_core_access;

protected:
    using super_t = detail::iterator_adaptor_base_t<
        Derived, Base, Value, Traversal, Reference, Difference
    >;

public:
    using base_type = Base;

    iterator_adaptor() = default;

    explicit iterator_adaptor(Base const& iter) :
        m_iterator(iter)
    {
    }

    base_type const& base() const { return m_iterator; }

protected:
    // for convenience in derived classes
    using iterator_adaptor_ = iterator_adaptor< Derived, Base, Value, Traversal, Reference, Difference >;

    //
    // lvalue access to the Base object for Derived
    //
    Base& base_reference() { return m_iterator; }
    Base const& base_reference() const { return m_iterator; }

private:
    //
    // Core iterator interface for iterator_facade.  This is private
    // to prevent temptation for Derived classes to use it, which
    // will often result in an error.  Derived classes should use
    // base_reference(), above, to get direct access to m_iterator.
    //
    typename super_t::reference dereference() const { return *m_iterator; }

    template< typename OtherDerived, typename OtherIterator, typename V, typename C, typename R, typename D >
    bool equal(iterator_adaptor< OtherDerived, OtherIterator, V, C, R, D > const& x) const
    {
        // Maybe readd with same_distance
        //           BOOST_STATIC_ASSERT(
        //               (detail::same_category_and_difference<Derived,OtherDerived>::value)
        //               );
        return m_iterator == x.base();
    }

    using my_traversal = typename iterator_category_to_traversal< typename super_t::iterator_category >::type;

    void advance(typename super_t::difference_type n)
    {
        static_assert(detail::is_traversal_at_least< my_traversal, random_access_traversal_tag >::value,
            "Iterator must support random access traversal.");
        m_iterator += n;
    }

    void increment() { ++m_iterator; }

    void decrement()
    {
        static_assert(detail::is_traversal_at_least< my_traversal, bidirectional_traversal_tag >::value,
            "Iterator must support bidirectional traversal.");
        --m_iterator;
    }

    template< typename OtherDerived, typename OtherIterator, typename V, typename C, typename R, typename D >
    typename super_t::difference_type distance_to(iterator_adaptor< OtherDerived, OtherIterator, V, C, R, D > const& y) const
    {
        static_assert(detail::is_traversal_at_least< my_traversal, random_access_traversal_tag >::value,
            "Super iterator must support random access traversal.");
        // Maybe readd with same_distance
        //           BOOST_STATIC_ASSERT(
        //               (detail::same_category_and_difference<Derived,OtherDerived>::value)
        //               );
        return y.base() - m_iterator;
    }

private: // data members
    Base m_iterator;
};

} // namespace iterators

using iterators::iterator_adaptor;

} // namespace boost

#include <boost/iterator/detail/config_undef.hpp>

#endif // BOOST_ITERATOR_ADAPTOR_23022003THW_HPP
