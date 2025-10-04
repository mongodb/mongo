// (C) Copyright David Abrahams 2002.
// (C) Copyright Jeremy Siek    2002.
// (C) Copyright Thomas Witt    2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_ITERATOR_FACADE_23022003THW_HPP
#define BOOST_ITERATOR_FACADE_23022003THW_HPP

#include <cstddef>
#include <memory>
#include <type_traits>

#include <boost/config.hpp>
#include <boost/mp11/utility.hpp>

#include <boost/iterator/interoperable.hpp>
#include <boost/iterator/iterator_traits.hpp>
#include <boost/iterator/iterator_categories.hpp>
#include <boost/iterator/detail/facade_iterator_category.hpp>
#include <boost/iterator/detail/type_traits/conjunction.hpp>
#include <boost/iterator/detail/type_traits/negation.hpp>

namespace boost {
namespace iterators {

// This forward declaration is required for the friend declaration
// in iterator_core_access
template<
    typename Derived,
    typename Value,
    typename CategoryOrTraversal,
    typename Reference   = Value&,
    typename Difference  = std::ptrdiff_t
>
class iterator_facade;

namespace detail {

// The type trait checks if the category or traversal is at least as advanced as the specified required traversal
template< typename CategoryOrTraversal, typename Required >
struct is_traversal_at_least :
    public std::is_convertible< typename iterator_category_to_traversal< CategoryOrTraversal >::type, Required >
{};

//
// enable if for use in operator implementation.
//
template<
    typename Facade1,
    typename Facade2,
    typename Return
>
struct enable_if_interoperable :
    public std::enable_if<
        is_interoperable< Facade1, Facade2 >::value,
        Return
    >
{};

//
// enable if for use in implementation of operators specific for random access traversal.
//
template<
    typename Facade1,
    typename Facade2,
    typename Return
>
struct enable_if_interoperable_and_random_access_traversal :
    public std::enable_if<
        detail::conjunction<
            is_interoperable< Facade1, Facade2 >,
            is_traversal_at_least< typename iterator_category< Facade1 >::type, random_access_traversal_tag >,
            is_traversal_at_least< typename iterator_category< Facade2 >::type, random_access_traversal_tag >
        >::value,
        Return
    >
{};

//
// Generates associated types for an iterator_facade with the
// given parameters.
//
template<
    typename ValueParam,
    typename CategoryOrTraversal,
    typename Reference,
    typename Difference
>
struct iterator_facade_types
{
    using iterator_category = typename facade_iterator_category<
        CategoryOrTraversal, ValueParam, Reference
    >::type;

    using value_type = typename std::remove_const< ValueParam >::type;

    // Not the real associated pointer type
    using pointer = typename std::add_pointer<
        typename std::conditional<
            boost::iterators::detail::iterator_writability_disabled< ValueParam, Reference >::value,
            const value_type,
            value_type
        >::type
    >::type;
};

// iterators whose dereference operators reference the same value
// for all iterators into the same sequence (like many input
// iterators) need help with their postfix ++: the referenced
// value must be read and stored away before the increment occurs
// so that *a++ yields the originally referenced element and not
// the next one.
template< typename Iterator >
class postfix_increment_proxy
{
    using value_type = typename iterator_value< Iterator >::type;

public:
    explicit postfix_increment_proxy(Iterator const& x) :
        stored_iterator(x),
        stored_value(*x)
    {}

    // Returning a mutable reference allows nonsense like
    // (*r++).mutate(), but it imposes fewer assumptions about the
    // behavior of the value_type.  In particular, recall that
    // (*r).mutate() is legal if operator* returns by value.
    // Provides readability of *r++
    value_type& operator*() const
    {
        return stored_value;
    }

    // Provides X(r++)
    operator Iterator const&() const
    {
        return stored_iterator;
    }

    // Provides (r++)->foo()
    value_type* operator->() const
    {
        return std::addressof(stored_value);
    }

private:
    Iterator stored_iterator;
    mutable value_type stored_value;
};


template< typename Iterator >
class writable_postfix_increment_dereference_proxy;

template< typename T >
struct is_not_writable_postfix_increment_dereference_proxy :
    public std::true_type
{};

template< typename Iterator >
struct is_not_writable_postfix_increment_dereference_proxy<
    writable_postfix_increment_dereference_proxy< Iterator >
> :
    public std::false_type
{};

template< typename Iterator >
class writable_postfix_increment_proxy;

//
// In general, we can't determine that such an iterator isn't
// writable -- we also need to store a copy of the old iterator so
// that it can be written into.
template< typename Iterator >
class writable_postfix_increment_dereference_proxy
{
    friend class writable_postfix_increment_proxy< Iterator >;

    using value_type = typename iterator_value< Iterator >::type;

public:
    explicit writable_postfix_increment_dereference_proxy(Iterator const& x) :
        stored_iterator(x),
        stored_value(*x)
    {}

    // Provides readability of *r++
    operator value_type&() const
    {
        return this->stored_value;
    }

    template< typename OtherIterator >
    writable_postfix_increment_dereference_proxy const&
    operator=(writable_postfix_increment_dereference_proxy< OtherIterator > const& x) const
    {
        typedef typename iterator_value< OtherIterator >::type other_value_type;
        *this->stored_iterator = static_cast< other_value_type& >(x);
        return *this;
    }

    // Provides writability of *r++
    template< typename T >
    typename std::enable_if<
        is_not_writable_postfix_increment_dereference_proxy< T >::value,
        writable_postfix_increment_dereference_proxy const&
    >::type operator=(T&& x) const
    {
        *this->stored_iterator = static_cast< T&& >(x);
        return *this;
    }

private:
    Iterator stored_iterator;
    mutable value_type stored_value;
};

template< typename Iterator >
class writable_postfix_increment_proxy
{
    using value_type = typename iterator_value< Iterator >::type;

public:
    explicit writable_postfix_increment_proxy(Iterator const& x) :
        dereference_proxy(x)
    {}

    writable_postfix_increment_dereference_proxy< Iterator > const&
    operator*() const
    {
        return dereference_proxy;
    }

    // Provides X(r++)
    operator Iterator const&() const
    {
        return dereference_proxy.stored_iterator;
    }

    // Provides (r++)->foo()
    value_type* operator->() const
    {
        return std::addressof(dereference_proxy.stored_value);
    }

private:
    writable_postfix_increment_dereference_proxy< Iterator > dereference_proxy;
};

template< typename Reference, typename Value >
struct is_non_proxy_reference :
    public std::is_convertible<
        typename std::remove_reference< Reference >::type const volatile*,
        Value const volatile*
    >
{};

// A metafunction to choose the result type of postfix ++
//
// Because the C++98 input iterator requirements say that *r++ has
// type T (value_type), implementations of some standard
// algorithms like lexicographical_compare may use constructions
// like:
//
//          *r++ < *s++
//
// If *r++ returns a proxy (as required if r is writable but not
// multipass), this sort of expression will fail unless the proxy
// supports the operator<.  Since there are any number of such
// operations, we're not going to try to support them.  Therefore,
// even if r++ returns a proxy, *r++ will only return a proxy if
// *r also returns a proxy.
template< typename Iterator, typename Value, typename Reference, typename CategoryOrTraversal >
struct postfix_increment_result
{
    using type = mp11::mp_eval_if_not<
        detail::conjunction<
            // A proxy is only needed for readable iterators
            std::is_convertible<
                Reference,
                // Use add_lvalue_reference to form `reference to Value` due to
                // some (strict) C++03 compilers (e.g. `gcc -std=c++03`) reject
                // 'reference-to-reference' in the template which described in CWG
                // DR106.
                // http://www.open-std.org/Jtc1/sc22/wg21/docs/cwg_defects.html#106
                typename std::add_lvalue_reference< Value const >::type
            >,

            // No multipass iterator can have values that disappear
            // before positions can be re-visited
            detail::negation<
                detail::is_traversal_at_least< CategoryOrTraversal, forward_traversal_tag >
            >
        >,
        Iterator,
        mp11::mp_if,
            is_non_proxy_reference< Reference, Value >,
            postfix_increment_proxy< Iterator >,
            writable_postfix_increment_proxy< Iterator >
    >;
};

// operator->() needs special support for input iterators to strictly meet the
// standard's requirements. If *i is not a reference type, we must still
// produce an lvalue to which a pointer can be formed.  We do that by
// returning a proxy object containing an instance of the reference object.
template< typename Reference, typename Pointer >
struct operator_arrow_dispatch // proxy references
{
    struct proxy
    {
        explicit proxy(Reference const& x) : m_ref(x) {}
        Reference* operator->() { return std::addressof(m_ref); }
        // This function is needed for MWCW and BCC, which won't call
        // operator-> again automatically per 13.3.1.2 para 8
        operator Reference*() { return std::addressof(m_ref); }
        Reference m_ref;
    };

    using result_type = proxy;

    static result_type apply(Reference const& x)
    {
        return result_type(x);
    }
};

template< typename T, typename Pointer >
struct operator_arrow_dispatch< T&, Pointer > // "real" references
{
    using result_type = Pointer;

    static result_type apply(T& x)
    {
        return std::addressof(x);
    }
};

// A proxy return type for operator[], needed to deal with
// iterators that may invalidate referents upon destruction.
// Consider the temporary iterator in *(a + n)
template< typename Iterator >
class operator_brackets_proxy
{
    // Iterator is actually an iterator_facade, so we do not have to
    // go through iterator_traits to access the traits.
    using reference = typename Iterator::reference;
    using value_type = typename Iterator::value_type;

public:
    operator_brackets_proxy(Iterator const& iter) :
        m_iter(iter)
    {}

    operator reference() const
    {
        return *m_iter;
    }

    operator_brackets_proxy& operator=(value_type const& val)
    {
        *m_iter = val;
        return *this;
    }

private:
    Iterator m_iter;
};

// A metafunction that determines whether operator[] must return a
// proxy, or whether it can simply return a copy of the value_type.
template< typename ValueType, typename Reference >
struct use_operator_brackets_proxy :
    public detail::negation<
        detail::conjunction<
            std::is_copy_constructible< ValueType >,
            std::is_trivial< ValueType >,
            iterator_writability_disabled< ValueType, Reference >
        >
    >
{};

template< typename Iterator, typename Value, typename Reference >
struct operator_brackets_result
{
    using type = typename std::conditional<
        use_operator_brackets_proxy<Value, Reference>::value,
        operator_brackets_proxy<Iterator>,
        Value
    >::type;
};

template< typename Iterator >
inline operator_brackets_proxy<Iterator> make_operator_brackets_result(Iterator const& iter, std::true_type)
{
    return operator_brackets_proxy< Iterator >(iter);
}

template< typename Iterator >
inline typename Iterator::value_type make_operator_brackets_result(Iterator const& iter, std::false_type)
{
    return *iter;
}

// A binary metafunction class that always returns bool.
template< typename Iterator1, typename Iterator2 >
using always_bool_t = bool;

template< typename Iterator1, typename Iterator2 >
using choose_difference_type_t = typename std::conditional<
    std::is_convertible< Iterator2, Iterator1 >::value,
    iterator_difference< Iterator1 >,
    iterator_difference< Iterator2 >
>::type::type;

template<
    typename Derived,
    typename Value,
    typename CategoryOrTraversal,
    typename Reference,
    typename Difference,
    bool IsBidirectionalTraversal,
    bool IsRandomAccessTraversal
>
class iterator_facade_base;

} // namespace detail


// Macros which describe the declarations of binary operators
#define BOOST_ITERATOR_FACADE_INTEROP_HEAD_IMPL(prefix, op, result_type, enabler)   \
    template<                                                           \
        typename Derived1, typename V1, typename TC1, typename Reference1, typename Difference1, \
        typename Derived2, typename V2, typename TC2, typename Reference2, typename Difference2  \
    >                                                                   \
    prefix typename enabler<                                            \
        Derived1, Derived2,                                             \
        result_type< Derived1, Derived2 >                               \
    >::type                                                             \
    operator op(                                                        \
        iterator_facade< Derived1, V1, TC1, Reference1, Difference1 > const& lhs,   \
        iterator_facade< Derived2, V2, TC2, Reference2, Difference2 > const& rhs)

#define BOOST_ITERATOR_FACADE_INTEROP_HEAD(prefix, op, result_type)       \
    BOOST_ITERATOR_FACADE_INTEROP_HEAD_IMPL(prefix, op, result_type, boost::iterators::detail::enable_if_interoperable)

#define BOOST_ITERATOR_FACADE_INTEROP_RANDOM_ACCESS_HEAD(prefix, op, result_type)       \
    BOOST_ITERATOR_FACADE_INTEROP_HEAD_IMPL(prefix, op, result_type, boost::iterators::detail::enable_if_interoperable_and_random_access_traversal)

#define BOOST_ITERATOR_FACADE_PLUS_HEAD(prefix,args)                \
    template< typename Derived, typename V, typename TC, typename R, typename D >   \
    prefix typename std::enable_if<                                 \
        boost::iterators::detail::is_traversal_at_least<            \
            TC,                                                     \
            boost::iterators::random_access_traversal_tag           \
        >::value,                                                   \
        Derived                                                     \
    >::type operator+ args

//
// Helper class for granting access to the iterator core interface.
//
// The simple core interface is used by iterator_facade. The core
// interface of a user/library defined iterator type should not be made public
// so that it does not clutter the public interface. Instead iterator_core_access
// should be made friend so that iterator_facade can access the core
// interface through iterator_core_access.
//
class iterator_core_access
{
    template< typename I, typename V, typename TC, typename R, typename D >
    friend class iterator_facade;
    template< typename I, typename V, typename TC, typename R, typename D, bool IsBidirectionalTraversal, bool IsRandomAccessTraversal >
    friend class detail::iterator_facade_base;

#define BOOST_ITERATOR_FACADE_RELATION(op)                                \
    BOOST_ITERATOR_FACADE_INTEROP_HEAD(friend, op, boost::iterators::detail::always_bool_t);

    BOOST_ITERATOR_FACADE_RELATION(==)
    BOOST_ITERATOR_FACADE_RELATION(!=)

#undef BOOST_ITERATOR_FACADE_RELATION

#define BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION(op)                                \
    BOOST_ITERATOR_FACADE_INTEROP_RANDOM_ACCESS_HEAD(friend, op, boost::iterators::detail::always_bool_t);

    BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION(<)
    BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION(>)
    BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION(<=)
    BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION(>=)

#undef BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION

    BOOST_ITERATOR_FACADE_INTEROP_RANDOM_ACCESS_HEAD(friend, -, boost::iterators::detail::choose_difference_type_t);

    BOOST_ITERATOR_FACADE_PLUS_HEAD(
        friend inline,
        (iterator_facade< Derived, V, TC, R, D > const&, typename Derived::difference_type)
    );

    BOOST_ITERATOR_FACADE_PLUS_HEAD(
        friend inline,
        (typename Derived::difference_type, iterator_facade< Derived, V, TC, R, D > const&)
    );

    template< typename Facade >
    static typename Facade::reference dereference(Facade const& f)
    {
        return f.dereference();
    }

    template< typename Facade >
    static void increment(Facade& f)
    {
        f.increment();
    }

    template< typename Facade >
    static void decrement(Facade& f)
    {
        f.decrement();
    }

    template< typename Facade1, typename Facade2 >
    static bool equal(Facade1 const& f1, Facade2 const& f2, std::true_type)
    {
        return f1.equal(f2);
    }

    template< typename Facade1, typename Facade2 >
    static bool equal(Facade1 const& f1, Facade2 const& f2, std::false_type)
    {
        return f2.equal(f1);
    }

    template< typename Facade >
    static void advance(Facade& f, typename Facade::difference_type n)
    {
        f.advance(n);
    }

    template< typename Facade1, typename Facade2 >
    static typename Facade1::difference_type distance_from(Facade1 const& f1, Facade2 const& f2, std::true_type)
    {
        return -f1.distance_to(f2);
    }

    template< typename Facade1, typename Facade2 >
    static typename Facade2::difference_type distance_from(Facade1 const& f1, Facade2 const& f2, std::false_type)
    {
        return f2.distance_to(f1);
    }

    //
    // Curiously Recurring Template interface.
    //
    template< typename I, typename V, typename TC, typename R, typename D >
    static I& derived(iterator_facade< I, V, TC, R, D >& facade)
    {
        return *static_cast< I* >(&facade);
    }

    template< typename I, typename V, typename TC, typename R, typename D >
    static I const& derived(iterator_facade< I, V, TC, R, D > const& facade)
    {
        return *static_cast< I const* >(&facade);
    }

    // objects of this class are useless
    iterator_core_access() = delete;
};

namespace detail {

// Implementation for forward traversal iterators
template<
    typename Derived,
    typename Value,
    typename CategoryOrTraversal,
    typename Reference,
    typename Difference
>
class iterator_facade_base< Derived, Value, CategoryOrTraversal, Reference, Difference, false, false >
{
private:
    using associated_types = boost::iterators::detail::iterator_facade_types<
        Value, CategoryOrTraversal, Reference, Difference
    >;

    using operator_arrow_dispatch_ = boost::iterators::detail::operator_arrow_dispatch<
        Reference,
        typename associated_types::pointer
    >;

public:
    using value_type = typename associated_types::value_type;
    using reference = Reference;
    using difference_type = Difference;

    using pointer = typename operator_arrow_dispatch_::result_type;

    using iterator_category = typename associated_types::iterator_category;

public:
    reference operator*() const
    {
        return iterator_core_access::dereference(this->derived());
    }

    pointer operator->() const
    {
        return operator_arrow_dispatch_::apply(*this->derived());
    }

    Derived& operator++()
    {
        iterator_core_access::increment(this->derived());
        return this->derived();
    }

protected:
    //
    // Curiously Recurring Template interface.
    //
    Derived& derived()
    {
        return *static_cast< Derived* >(this);
    }

    Derived const& derived() const
    {
        return *static_cast< Derived const* >(this);
    }
};

// Implementation for bidirectional traversal iterators
template<
    typename Derived,
    typename Value,
    typename CategoryOrTraversal,
    typename Reference,
    typename Difference
>
class iterator_facade_base< Derived, Value, CategoryOrTraversal, Reference, Difference, true, false > :
    public iterator_facade_base< Derived, Value, CategoryOrTraversal, Reference, Difference, false, false >
{
public:
    Derived& operator--()
    {
        iterator_core_access::decrement(this->derived());
        return this->derived();
    }

    Derived operator--(int)
    {
        Derived tmp(this->derived());
        --*this;
        return tmp;
    }
};

// Implementation for random access traversal iterators
template<
    typename Derived,
    typename Value,
    typename CategoryOrTraversal,
    typename Reference,
    typename Difference
>
class iterator_facade_base< Derived, Value, CategoryOrTraversal, Reference, Difference, true, true > :
    public iterator_facade_base< Derived, Value, CategoryOrTraversal, Reference, Difference, true, false >
{
private:
    using base_type = iterator_facade_base< Derived, Value, CategoryOrTraversal, Reference, Difference, true, false >;

public:
    using reference = typename base_type::reference;
    using difference_type = typename base_type::difference_type;

public:
    typename boost::iterators::detail::operator_brackets_result< Derived, Value, reference >::type
    operator[](difference_type n) const
    {
        return boost::iterators::detail::make_operator_brackets_result< Derived >(
            this->derived() + n,
            std::integral_constant< bool, boost::iterators::detail::use_operator_brackets_proxy< Value, Reference >::value >{}
        );
    }

    Derived& operator+=(difference_type n)
    {
        iterator_core_access::advance(this->derived(), n);
        return this->derived();
    }

    Derived& operator-=(difference_type n)
    {
        iterator_core_access::advance(this->derived(), -n);
        return this->derived();
    }

    Derived operator-(difference_type x) const
    {
        Derived result(this->derived());
        return result -= x;
    }
};

} // namespace detail

//
// iterator_facade - use as a public base class for defining new
// standard-conforming iterators.
//
template<
    typename Derived,             // The derived iterator type being constructed
    typename Value,
    typename CategoryOrTraversal,
    typename Reference,
    typename Difference
>
class iterator_facade :
    public detail::iterator_facade_base<
        Derived,
        Value,
        CategoryOrTraversal,
        Reference,
        Difference,
        detail::is_traversal_at_least< CategoryOrTraversal, bidirectional_traversal_tag >::value,
        detail::is_traversal_at_least< CategoryOrTraversal, random_access_traversal_tag >::value
    >
{
protected:
    // For use by derived classes
    using iterator_facade_ = iterator_facade< Derived, Value, CategoryOrTraversal, Reference, Difference >;
};

template< typename I, typename V, typename TC, typename R, typename D >
inline typename boost::iterators::detail::postfix_increment_result< I, V, R, TC >::type
operator++(iterator_facade< I, V, TC, R, D >& i, int)
{
    typename boost::iterators::detail::postfix_increment_result< I, V, R, TC >::type
        tmp(*static_cast< I* >(&i));

    ++i;

    return tmp;
}


//
// Comparison operator implementation. The library supplied operators
// enables the user to provide fully interoperable constant/mutable
// iterator types. I.e. the library provides all operators
// for all mutable/constant iterator combinations.
//
// Note though that this kind of interoperability for constant/mutable
// iterators is not required by the standard for container iterators.
// All the standard asks for is a conversion mutable -> constant.
// Most standard library implementations nowadays provide fully interoperable
// iterator implementations, but there are still heavily used implementations
// that do not provide them. (Actually it's even worse, they do not provide
// them for only a few iterators.)
//
// ?? Maybe a BOOST_ITERATOR_NO_FULL_INTEROPERABILITY macro should
//    enable the user to turn off mixed type operators
//
// The library takes care to provide only the right operator overloads.
// I.e.
//
// bool operator==(Iterator,      Iterator);
// bool operator==(ConstIterator, Iterator);
// bool operator==(Iterator,      ConstIterator);
// bool operator==(ConstIterator, ConstIterator);
//
//   ...
//
// In order to do so it uses c++ idioms that are not yet widely supported
// by current compiler releases. The library is designed to degrade gracefully
// in the face of compiler deficiencies. In general compiler
// deficiencies result in less strict error checking and more obscure
// error messages, functionality is not affected.
//
// For full operation compiler support for "Substitution Failure Is Not An Error"
// (aka. enable_if) and boost::is_convertible is required.
//
// The following problems occur if support is lacking.
//
// Pseudo code
//
// ---------------
// AdaptorA<Iterator1> a1;
// AdaptorA<Iterator2> a2;
//
// // This will result in a no such overload error in full operation
// // If enable_if or is_convertible is not supported
// // The instantiation will fail with an error hopefully indicating that
// // there is no operator== for Iterator1, Iterator2
// // The same will happen if no enable_if is used to remove
// // false overloads from the templated conversion constructor
// // of AdaptorA.
//
// a1 == a2;
// ----------------
//
// AdaptorA<Iterator> a;
// AdaptorB<Iterator> b;
//
// // This will result in a no such overload error in full operation
// // If enable_if is not supported the static assert used
// // in the operator implementation will fail.
// // This will accidently work if is_convertible is not supported.
//
// a == b;
// ----------------
//

#define BOOST_ITERATOR_FACADE_INTEROP(op, result_type, return_prefix, base_op)                 \
    BOOST_ITERATOR_FACADE_INTEROP_HEAD(inline, op, result_type)                                \
    {                                                                                          \
        return_prefix iterator_core_access::base_op(                                           \
            *static_cast< Derived1 const* >(&lhs),                                             \
            *static_cast< Derived2 const* >(&rhs),                                             \
            std::integral_constant< bool, std::is_convertible< Derived2, Derived1 >::value >() \
        );                                                                                     \
    }

#define BOOST_ITERATOR_FACADE_RELATION(op, return_prefix, base_op) \
    BOOST_ITERATOR_FACADE_INTEROP(                                 \
        op,                                                        \
        boost::iterators::detail::always_bool_t,                   \
        return_prefix,                                             \
        base_op                                                    \
    )

BOOST_ITERATOR_FACADE_RELATION(==, return, equal)
BOOST_ITERATOR_FACADE_RELATION(!=, return !, equal)

#undef BOOST_ITERATOR_FACADE_RELATION


#define BOOST_ITERATOR_FACADE_INTEROP_RANDOM_ACCESS(op, result_type, return_prefix, base_op)   \
    BOOST_ITERATOR_FACADE_INTEROP_RANDOM_ACCESS_HEAD(inline, op, result_type)                  \
    {                                                                                          \
        return_prefix iterator_core_access::base_op(                                           \
            *static_cast< Derived1 const* >(&lhs),                                             \
            *static_cast< Derived2 const* >(&rhs),                                             \
            std::integral_constant< bool, std::is_convertible< Derived2, Derived1 >::value >() \
        );                                                                                     \
    }

#define BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION(op, return_prefix, base_op) \
    BOOST_ITERATOR_FACADE_INTEROP_RANDOM_ACCESS(                                 \
        op,                                                                      \
        boost::iterators::detail::always_bool_t,                                 \
        return_prefix,                                                           \
        base_op                                                                  \
    )

BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION(<, return 0 >, distance_from)
BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION(>, return 0 <, distance_from)
BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION(<=, return 0 >=, distance_from)
BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION(>=, return 0 <=, distance_from)

#undef BOOST_ITERATOR_FACADE_RANDOM_ACCESS_RELATION

// operator- requires an additional part in the static assertion
BOOST_ITERATOR_FACADE_INTEROP_RANDOM_ACCESS(
    -,
    boost::iterators::detail::choose_difference_type_t,
    return,
    distance_from
)

#undef BOOST_ITERATOR_FACADE_INTEROP
#undef BOOST_ITERATOR_FACADE_INTEROP_RANDOM_ACCESS

#define BOOST_ITERATOR_FACADE_PLUS(args)               \
    BOOST_ITERATOR_FACADE_PLUS_HEAD(inline, args)      \
    {                                                  \
        Derived tmp(static_cast< Derived const& >(i)); \
        return tmp += n;                               \
    }

BOOST_ITERATOR_FACADE_PLUS((iterator_facade< Derived, V, TC, R, D > const& i, typename Derived::difference_type n))
BOOST_ITERATOR_FACADE_PLUS((typename Derived::difference_type n, iterator_facade< Derived, V, TC, R, D > const& i))

#undef BOOST_ITERATOR_FACADE_PLUS
#undef BOOST_ITERATOR_FACADE_PLUS_HEAD

#undef BOOST_ITERATOR_FACADE_INTEROP_HEAD
#undef BOOST_ITERATOR_FACADE_INTEROP_RANDOM_ACCESS_HEAD
#undef BOOST_ITERATOR_FACADE_INTEROP_HEAD_IMPL

} // namespace iterators

using iterators::iterator_core_access;
using iterators::iterator_facade;

} // namespace boost

#endif // BOOST_ITERATOR_FACADE_23022003THW_HPP
