#ifndef BOOST_SYSTEM_RESULT_HPP_INCLUDED
#define BOOST_SYSTEM_RESULT_HPP_INCLUDED

// Copyright 2017, 2021, 2022 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/system/errc.hpp>
#include <boost/system/system_error.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/detail/error_category_impl.hpp>
#include <boost/variant2/variant.hpp>
#include <boost/throw_exception.hpp>
#include <boost/assert/source_location.hpp>
#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <type_traits>
#include <utility>
#include <iosfwd>
#include <system_error>
#include <exception>

//

namespace boost
{
namespace system
{

// throw_exception_from_error

#if defined(__GNUC__) && __GNUC__ >= 7 && __GNUC__ <= 8
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wattributes"
#endif

BOOST_NORETURN BOOST_NOINLINE inline void throw_exception_from_error( error_code const & e, boost::source_location const& loc )
{
    boost::throw_with_location( system_error( e ), loc );
}

BOOST_NORETURN BOOST_NOINLINE inline void throw_exception_from_error( errc::errc_t const & e, boost::source_location const& loc )
{
    boost::throw_with_location( system_error( make_error_code( e ) ), loc );
}

BOOST_NORETURN BOOST_NOINLINE inline void throw_exception_from_error( std::error_code const & e, boost::source_location const& loc )
{
    boost::throw_with_location( std::system_error( e ), loc );
}

BOOST_NORETURN BOOST_NOINLINE inline void throw_exception_from_error( std::errc const & e, boost::source_location const& loc )
{
    boost::throw_with_location( std::system_error( make_error_code( e ) ), loc );
}

BOOST_NORETURN BOOST_NOINLINE inline void throw_exception_from_error( std::exception_ptr const & p, boost::source_location const& loc )
{
    if( p )
    {
        std::rethrow_exception( p );
    }
    else
    {
        boost::throw_with_location( std::bad_exception(), loc );
    }
}

#if defined(__GNUC__) && __GNUC__ >= 7 && __GNUC__ <= 8
# pragma GCC diagnostic pop
#endif

// in_place_*

using in_place_value_t = variant2::in_place_index_t<0>;
BOOST_INLINE_CONSTEXPR in_place_value_t in_place_value{};

using in_place_error_t = variant2::in_place_index_t<1>;
BOOST_INLINE_CONSTEXPR in_place_error_t in_place_error{};

namespace detail
{

template<class T> using remove_cvref = typename std::remove_cv< typename std::remove_reference<T>::type >::type;

template<class... T> using is_errc_t = std::is_same<mp11::mp_list<remove_cvref<T>...>, mp11::mp_list<errc::errc_t>>;

template<class T, class... A> struct is_constructible: std::is_constructible<T, A...> {};
template<class A> struct is_constructible<bool, A>: std::is_convertible<A, bool> {};
template<class A> struct is_constructible<bool const, A>: std::is_convertible<A, bool> {};

} // namespace detail

// result

template<class T, class E = error_code> class result
{
private:

    variant2::variant<T, E> v_;

public:

    using value_type = T;
    using error_type = E;

    static constexpr in_place_value_t in_place_value{};
    static constexpr in_place_error_t in_place_error{};

public:

    // constructors

    // default
    template<class En2 = void, class En = typename std::enable_if<
        std::is_void<En2>::value &&
        std::is_default_constructible<T>::value
        >::type>
    constexpr result()
        noexcept( std::is_nothrow_default_constructible<T>::value )
        : v_( in_place_value )
    {
    }

    // implicit, value
    template<class A = T, typename std::enable_if<
        std::is_convertible<A, T>::value &&
        !(detail::is_errc_t<A>::value && std::is_arithmetic<T>::value) &&
        !std::is_convertible<A, E>::value, int>::type = 0>
    constexpr result( A&& a )
        noexcept( std::is_nothrow_constructible<T, A>::value )
        : v_( in_place_value, std::forward<A>(a) )
    {
    }

    // implicit, error
    template<class A = E, class = void, typename std::enable_if<
        std::is_convertible<A, E>::value &&
        !std::is_convertible<A, T>::value, int>::type = 0>
    constexpr result( A&& a )
        noexcept( std::is_nothrow_constructible<E, A>::value )
        : v_( in_place_error, std::forward<A>(a) )
    {
    }

    // explicit, value
    template<class... A, class En = typename std::enable_if<
        detail::is_constructible<T, A...>::value &&
        !(detail::is_errc_t<A...>::value && std::is_arithmetic<T>::value) &&
        !detail::is_constructible<E, A...>::value &&
        sizeof...(A) >= 1
        >::type>
    explicit constexpr result( A&&... a )
        noexcept( std::is_nothrow_constructible<T, A...>::value )
        : v_( in_place_value, std::forward<A>(a)... )
    {
    }

    // explicit, error
    template<class... A, class En2 = void, class En = typename std::enable_if<
        !detail::is_constructible<T, A...>::value &&
        detail::is_constructible<E, A...>::value &&
        sizeof...(A) >= 1
        >::type>
    explicit constexpr result( A&&... a )
        noexcept( std::is_nothrow_constructible<E, A...>::value )
        : v_( in_place_error, std::forward<A>(a)... )
    {
    }

    // tagged, value
    template<class... A, class En = typename std::enable_if<
        std::is_constructible<T, A...>::value
        >::type>
    constexpr result( in_place_value_t, A&&... a )
        noexcept( std::is_nothrow_constructible<T, A...>::value )
        : v_( in_place_value, std::forward<A>(a)... )
    {
    }

    // tagged, error
    template<class... A, class En = typename std::enable_if<
        std::is_constructible<E, A...>::value
        >::type>
    constexpr result( in_place_error_t, A&&... a )
        noexcept( std::is_nothrow_constructible<E, A...>::value )
        : v_( in_place_error, std::forward<A>(a)... )
    {
    }

    // converting
    template<class T2, class E2, class En = typename std::enable_if<
        std::is_convertible<T2, T>::value &&
        std::is_convertible<E2, E>::value &&
        !std::is_convertible<result<T2, E2> const&, T>::value
        >::type>
    BOOST_CXX14_CONSTEXPR result( result<T2, E2> const& r2 )
        noexcept(
            std::is_nothrow_constructible<T, T2 const&>::value &&
            std::is_nothrow_constructible<E, E2>::value &&
            std::is_nothrow_default_constructible<E2>::value &&
            std::is_nothrow_copy_constructible<E2>::value )
        : v_( in_place_error, r2.error() )
    {
        if( r2 )
        {
            v_.template emplace<0>( *r2 );
        }
    }

    template<class T2, class E2, class En = typename std::enable_if<
        std::is_convertible<T2, T>::value &&
        std::is_convertible<E2, E>::value &&
        !std::is_convertible<result<T2, E2>&&, T>::value
        >::type>
    BOOST_CXX14_CONSTEXPR result( result<T2, E2>&& r2 )
        noexcept(
            std::is_nothrow_constructible<T, T2&&>::value &&
            std::is_nothrow_constructible<E, E2>::value &&
            std::is_nothrow_default_constructible<E2>::value &&
            std::is_nothrow_copy_constructible<E2>::value )
        : v_( in_place_error, r2.error() )
    {
        if( r2 )
        {
            v_.template emplace<0>( std::move( *r2 ) );
        }
    }

    // queries

    constexpr bool has_value() const noexcept
    {
        return v_.index() == 0;
    }

    constexpr bool has_error() const noexcept
    {
        return v_.index() == 1;
    }

    constexpr explicit operator bool() const noexcept
    {
        return v_.index() == 0;
    }

    // checked value access
#if defined( BOOST_NO_CXX11_REF_QUALIFIERS )

    BOOST_CXX14_CONSTEXPR T value( boost::source_location const& loc = BOOST_CURRENT_LOCATION ) const
    {
        if( has_value() )
        {
            return variant2::unsafe_get<0>( v_ );
        }
        else
        {
            throw_exception_from_error( variant2::unsafe_get<1>( v_ ), loc );
        }
    }

#else

    BOOST_CXX14_CONSTEXPR T& value( boost::source_location const& loc = BOOST_CURRENT_LOCATION ) &
    {
        if( has_value() )
        {
            return variant2::unsafe_get<0>( v_ );
        }
        else
        {
            throw_exception_from_error( variant2::unsafe_get<1>( v_ ), loc );
        }
    }

    BOOST_CXX14_CONSTEXPR T const& value( boost::source_location const& loc = BOOST_CURRENT_LOCATION ) const&
    {
        if( has_value() )
        {
            return variant2::unsafe_get<0>( v_ );
        }
        else
        {
            throw_exception_from_error( variant2::unsafe_get<1>( v_ ), loc );
        }
    }

    template<class U = T>
        BOOST_CXX14_CONSTEXPR
        typename std::enable_if<std::is_move_constructible<U>::value, T>::type
        value( boost::source_location const& loc = BOOST_CURRENT_LOCATION ) &&
    {
        return std::move( value( loc ) );
    }

    template<class U = T>
        BOOST_CXX14_CONSTEXPR
        typename std::enable_if<!std::is_move_constructible<U>::value, T&&>::type
        value( boost::source_location const& loc = BOOST_CURRENT_LOCATION ) &&
    {
        return std::move( value( loc ) );
    }

    template<class U = T>
        BOOST_CXX14_CONSTEXPR
        typename std::enable_if<std::is_move_constructible<U>::value, T>::type
        value() const && = delete;

    template<class U = T>
        BOOST_CXX14_CONSTEXPR
        typename std::enable_if<!std::is_move_constructible<U>::value, T const&&>::type
        value( boost::source_location const& loc = BOOST_CURRENT_LOCATION ) const &&
    {
        return std::move( value( loc ) );
    }

#endif

    // unchecked value access

    BOOST_CXX14_CONSTEXPR T* operator->() noexcept
    {
        return variant2::get_if<0>( &v_ );
    }

    BOOST_CXX14_CONSTEXPR T const* operator->() const noexcept
    {
        return variant2::get_if<0>( &v_ );
    }

#if defined( BOOST_NO_CXX11_REF_QUALIFIERS )

    BOOST_CXX14_CONSTEXPR T& operator*() noexcept
    {
        BOOST_ASSERT( has_value() );
        return *operator->();
    }

    BOOST_CXX14_CONSTEXPR T const& operator*() const noexcept
    {
        BOOST_ASSERT( has_value() );
        return *operator->();
    }

#else

    BOOST_CXX14_CONSTEXPR T& operator*() & noexcept
    {
        BOOST_ASSERT( has_value() );
        return *operator->();
    }

    BOOST_CXX14_CONSTEXPR T const& operator*() const & noexcept
    {
        BOOST_ASSERT( has_value() );
        return *operator->();
    }

    template<class U = T>
        BOOST_CXX14_CONSTEXPR
        typename std::enable_if<std::is_move_constructible<U>::value, T>::type
        operator*() && noexcept(std::is_nothrow_move_constructible<T>::value)
    {
        return std::move(**this);
    }

    template<class U = T>
        BOOST_CXX14_CONSTEXPR
        typename std::enable_if<!std::is_move_constructible<U>::value, T&&>::type
        operator*() && noexcept
    {
        return std::move(**this);
    }

    template<class U = T>
        BOOST_CXX14_CONSTEXPR
        typename std::enable_if<std::is_move_constructible<U>::value, T>::type
        operator*() const && noexcept = delete;

    template<class U = T>
        BOOST_CXX14_CONSTEXPR
        typename std::enable_if<!std::is_move_constructible<U>::value, T const&&>::type
        operator*() const && noexcept
    {
        return std::move(**this);
    }

#endif

    // error access

    constexpr E error() const &
        noexcept( std::is_nothrow_default_constructible<E>::value && std::is_nothrow_copy_constructible<E>::value )
    {
        return has_error()? variant2::unsafe_get<1>( v_ ): E();
    }

    BOOST_CXX14_CONSTEXPR E error() &&
        noexcept( std::is_nothrow_default_constructible<E>::value && std::is_nothrow_move_constructible<E>::value )
    {
        return has_error()? std::move( variant2::unsafe_get<1>( v_ ) ): E();
    }

    // emplace

    template<class... A>
    BOOST_CXX14_CONSTEXPR T& emplace( A&&... a )
    {
        return v_.template emplace<0>( std::forward<A>(a)... );
    }

    // swap

    BOOST_CXX14_CONSTEXPR void swap( result& r )
        noexcept( noexcept( v_.swap( r.v_ ) ) )
    {
        v_.swap( r.v_ );
    }

    friend BOOST_CXX14_CONSTEXPR void swap( result & r1, result & r2 )
        noexcept( noexcept( r1.swap( r2 ) ) )
    {
        r1.swap( r2 );
    }

    // equality

    friend constexpr bool operator==( result const & r1, result const & r2 )
        noexcept( noexcept( r1.v_ == r2.v_ ) )
    {
        return r1.v_ == r2.v_;
    }

    friend constexpr bool operator!=( result const & r1, result const & r2 )
        noexcept( noexcept( !( r1 == r2 ) ) )
    {
        return !( r1 == r2 );
    }
};

#if defined(BOOST_NO_CXX17_INLINE_VARIABLES)

template<class T, class E> constexpr in_place_value_t result<T, E>::in_place_value;
template<class T, class E> constexpr in_place_error_t result<T, E>::in_place_error;

#endif

template<class Ch, class Tr, class T, class E> std::basic_ostream<Ch, Tr>& operator<<( std::basic_ostream<Ch, Tr>& os, result<T, E> const & r )
{
    if( r.has_value() )
    {
        os << "value:" << *r;
    }
    else
    {
        os << "error:" << r.error();
    }

    return os;
}

// result<void>

template<class E> class result<void, E>
{
private:

    variant2::variant<variant2::monostate, E> v_;

public:

    using value_type = void;
    using error_type = E;

    static constexpr in_place_value_t in_place_value{};
    static constexpr in_place_error_t in_place_error{};

public:

    // constructors

    // default
    constexpr result() noexcept
        : v_( in_place_value )
    {
    }

    // explicit, error
    template<class A, class En = typename std::enable_if<
        std::is_constructible<E, A>::value &&
        !std::is_convertible<A, E>::value
        >::type>
    explicit constexpr result( A&& a )
        noexcept( std::is_nothrow_constructible<E, A>::value )
        : v_( in_place_error, std::forward<A>(a) )
    {
    }

    // implicit, error
    template<class A, class En2 = void, class En = typename std::enable_if<
        std::is_convertible<A, E>::value
        >::type>
    constexpr result( A&& a )
        noexcept( std::is_nothrow_constructible<E, A>::value )
        : v_( in_place_error, std::forward<A>(a) )
    {
    }

    // more than one arg, error
    template<class... A, class En2 = void, class En3 = void, class En = typename std::enable_if<
        std::is_constructible<E, A...>::value &&
        sizeof...(A) >= 2
        >::type>
    constexpr result( A&&... a )
        noexcept( std::is_nothrow_constructible<E, A...>::value )
        : v_( in_place_error, std::forward<A>(a)... )
    {
    }

    // tagged, value
    constexpr result( in_place_value_t ) noexcept
        : v_( in_place_value )
    {
    }

    // tagged, error
    template<class... A, class En = typename std::enable_if<
        std::is_constructible<E, A...>::value
        >::type>
    constexpr result( in_place_error_t, A&&... a )
        noexcept( std::is_nothrow_constructible<E, A...>::value )
        : v_( in_place_error, std::forward<A>(a)... )
    {
    }

    // converting
    template<class E2, class En = typename std::enable_if<
        std::is_convertible<E2, E>::value
        >::type>
    BOOST_CXX14_CONSTEXPR result( result<void, E2> const& r2 )
        noexcept(
            std::is_nothrow_constructible<E, E2>::value &&
            std::is_nothrow_default_constructible<E2>::value &&
            std::is_nothrow_copy_constructible<E2>::value )
        : v_( in_place_error, r2.error() )
    {
        if( r2 )
        {
            this->emplace();
        }
    }

    // queries

    constexpr bool has_value() const noexcept
    {
        return v_.index() == 0;
    }

    constexpr bool has_error() const noexcept
    {
        return v_.index() == 1;
    }

    constexpr explicit operator bool() const noexcept
    {
        return v_.index() == 0;
    }

    // checked value access

    BOOST_CXX14_CONSTEXPR void value( boost::source_location const& loc = BOOST_CURRENT_LOCATION ) const
    {
        if( has_value() )
        {
        }
        else
        {
            throw_exception_from_error( variant2::unsafe_get<1>( v_ ), loc );
        }
    }

    // unchecked value access

    BOOST_CXX14_CONSTEXPR void* operator->() noexcept
    {
        return variant2::get_if<0>( &v_ );
    }

    BOOST_CXX14_CONSTEXPR void const* operator->() const noexcept
    {
        return variant2::get_if<0>( &v_ );
    }

    BOOST_CXX14_CONSTEXPR void operator*() const noexcept
    {
        BOOST_ASSERT( has_value() );
    }

    // error access

    constexpr E error() const &
        noexcept( std::is_nothrow_default_constructible<E>::value && std::is_nothrow_copy_constructible<E>::value )
    {
        return has_error()? variant2::unsafe_get<1>( v_ ): E();
    }

    BOOST_CXX14_CONSTEXPR E error() &&
        noexcept( std::is_nothrow_default_constructible<E>::value && std::is_nothrow_move_constructible<E>::value )
    {
        return has_error()? std::move( variant2::unsafe_get<1>( v_ ) ): E();
    }

    // emplace

    BOOST_CXX14_CONSTEXPR void emplace()
    {
        v_.template emplace<0>();
    }

    // swap

    BOOST_CXX14_CONSTEXPR void swap( result& r )
        noexcept( noexcept( v_.swap( r.v_ ) ) )
    {
        v_.swap( r.v_ );
    }

    friend BOOST_CXX14_CONSTEXPR void swap( result & r1, result & r2 )
        noexcept( noexcept( r1.swap( r2 ) ) )
    {
        r1.swap( r2 );
    }

    // equality

    friend constexpr bool operator==( result const & r1, result const & r2 )
        noexcept( noexcept( r1.v_ == r2.v_ ) )
    {
        return r1.v_ == r2.v_;
    }

    friend constexpr bool operator!=( result const & r1, result const & r2 )
        noexcept( noexcept( !( r1 == r2 ) ) )
    {
        return !( r1 == r2 );
    }
};

#if defined(BOOST_NO_CXX17_INLINE_VARIABLES)

template<class E> constexpr in_place_value_t result<void, E>::in_place_value;
template<class E> constexpr in_place_error_t result<void, E>::in_place_error;

#endif

template<class Ch, class Tr, class E> std::basic_ostream<Ch, Tr>& operator<<( std::basic_ostream<Ch, Tr>& os, result<void, E> const & r )
{
    if( r.has_value() )
    {
        os << "value:void";
    }
    else
    {
        os << "error:" << r.error();
    }

    return os;
}

// result<T&, E>

namespace detail
{

template<class U, class A> struct reference_to_temporary: std::integral_constant<bool,
    !std::is_reference<A>::value ||
    !std::is_convertible<typename std::remove_reference<A>::type*, U*>::value
> {};

} // namespace detail

template<class U, class E> class result<U&, E>
{
private:

    variant2::variant<U*, E> v_;

public:

    using value_type = U&;
    using error_type = E;

    static constexpr in_place_value_t in_place_value{};
    static constexpr in_place_error_t in_place_error{};

public:

    // constructors

    // implicit, value
    template<class A, typename std::enable_if<
        std::is_convertible<A, U&>::value &&
        !detail::reference_to_temporary<U, A>::value &&
        !std::is_convertible<A, E>::value, int>::type = 0>
    constexpr result( A&& a )
        noexcept( std::is_nothrow_constructible<U&, A>::value )
        : v_( in_place_value, &static_cast<U&>( std::forward<A>(a) ) )
    {
    }

    // implicit, error
    template<class A = E, class = void, typename std::enable_if<
        std::is_convertible<A, E>::value &&
        !std::is_convertible<A, U&>::value, int>::type = 0>
    constexpr result( A&& a )
        noexcept( std::is_nothrow_constructible<E, A>::value )
        : v_( in_place_error, std::forward<A>(a) )
    {
    }

    // explicit, value
    template<class A, class En = typename std::enable_if<
        detail::is_constructible<U&, A>::value &&
        !std::is_convertible<A, U&>::value &&
        !detail::reference_to_temporary<U, A>::value &&
        !detail::is_constructible<E, A>::value
        >::type>
    explicit constexpr result( A&& a )
        noexcept( std::is_nothrow_constructible<U&, A>::value )
        : v_( in_place_value, &static_cast<U&>( std::forward<A>(a) ) )
    {
    }

    // explicit, error
    template<class... A, class En2 = void, class En = typename std::enable_if<
        !detail::is_constructible<U&, A...>::value &&
        detail::is_constructible<E, A...>::value &&
        sizeof...(A) >= 1
        >::type>
    explicit constexpr result( A&&... a )
        noexcept( std::is_nothrow_constructible<E, A...>::value )
        : v_( in_place_error, std::forward<A>(a)... )
    {
    }

    // tagged, value
    template<class A, class En = typename std::enable_if<
        std::is_constructible<U&, A>::value &&
        !detail::reference_to_temporary<U, A>::value
        >::type>
    constexpr result( in_place_value_t, A&& a )
        noexcept( std::is_nothrow_constructible<U&, A>::value )
        : v_( in_place_value, &static_cast<U&>( std::forward<A>(a) ) )
    {
    }

    // tagged, error
    template<class... A, class En = typename std::enable_if<
        std::is_constructible<E, A...>::value
        >::type>
    constexpr result( in_place_error_t, A&&... a )
        noexcept( std::is_nothrow_constructible<E, A...>::value )
        : v_( in_place_error, std::forward<A>(a)... )
    {
    }

    // converting
    template<class U2, class E2, class En = typename std::enable_if<
        std::is_convertible<U2&, U&>::value &&
        !detail::reference_to_temporary<U, U2&>::value &&
        std::is_convertible<E2, E>::value &&
        !std::is_convertible<result<U2&, E2> const&, U&>::value
        >::type>
    BOOST_CXX14_CONSTEXPR result( result<U2&, E2> const& r2 )
        noexcept(
            std::is_nothrow_constructible<U&, U2&>::value &&
            std::is_nothrow_constructible<E, E2>::value &&
            std::is_nothrow_default_constructible<E2>::value &&
            std::is_nothrow_copy_constructible<E2>::value )
        : v_( in_place_error, r2.error() )
    {
        if( r2 )
        {
            this->emplace( *r2 );
        }
    }

    // queries

    constexpr bool has_value() const noexcept
    {
        return v_.index() == 0;
    }

    constexpr bool has_error() const noexcept
    {
        return v_.index() == 1;
    }

    constexpr explicit operator bool() const noexcept
    {
        return v_.index() == 0;
    }

    // checked value access

    BOOST_CXX14_CONSTEXPR U& value( boost::source_location const& loc = BOOST_CURRENT_LOCATION ) const
    {
        if( has_value() )
        {
            return *variant2::unsafe_get<0>( v_ );
        }
        else
        {
            throw_exception_from_error( variant2::unsafe_get<1>( v_ ), loc );
        }
    }

    // unchecked value access

    BOOST_CXX14_CONSTEXPR U* operator->() const noexcept
    {
        return has_value()? variant2::unsafe_get<0>( v_ ): 0;
    }

    BOOST_CXX14_CONSTEXPR U& operator*() const noexcept
    {
        BOOST_ASSERT( has_value() );
        return *operator->();
    }

    // error access

    constexpr E error() const &
        noexcept( std::is_nothrow_default_constructible<E>::value && std::is_nothrow_copy_constructible<E>::value )
    {
        return has_error()? variant2::unsafe_get<1>( v_ ): E();
    }

    BOOST_CXX14_CONSTEXPR E error() &&
        noexcept( std::is_nothrow_default_constructible<E>::value && std::is_nothrow_move_constructible<E>::value )
    {
        return has_error()? std::move( variant2::unsafe_get<1>( v_ ) ): E();
    }

    // emplace

    template<class A, class En = typename std::enable_if<
        detail::is_constructible<U&, A>::value &&
        !detail::reference_to_temporary<U, A>::value
        >::type>
    BOOST_CXX14_CONSTEXPR U& emplace( A&& a )
    {
        return *v_.template emplace<0>( &static_cast<U&>( a ) );
    }

    // swap

    BOOST_CXX14_CONSTEXPR void swap( result& r )
        noexcept( noexcept( v_.swap( r.v_ ) ) )
    {
        v_.swap( r.v_ );
    }

    friend BOOST_CXX14_CONSTEXPR void swap( result & r1, result & r2 )
        noexcept( noexcept( r1.swap( r2 ) ) )
    {
        r1.swap( r2 );
    }

    // equality

    friend constexpr bool operator==( result const & r1, result const & r2 )
        noexcept( noexcept( r1 && r2? *r1 == *r2: r1.v_ == r2.v_ ) )
    {
        return r1 && r2? *r1 == *r2: r1.v_ == r2.v_;
    }

    friend constexpr bool operator!=( result const & r1, result const & r2 )
        noexcept( noexcept( !( r1 == r2 ) ) )
    {
        return !( r1 == r2 );
    }
};

#if defined(BOOST_NO_CXX17_INLINE_VARIABLES)

template<class U, class E> constexpr in_place_value_t result<U&, E>::in_place_value;
template<class U, class E> constexpr in_place_error_t result<U&, E>::in_place_error;

#endif

// operator|

namespace detail
{

// is_value_convertible_to

template<class T, class U> struct is_value_convertible_to: std::is_convertible<T, U>
{
};

template<class T, class U> struct is_value_convertible_to<T, U&>:
    std::integral_constant<bool,
        std::is_lvalue_reference<T>::value &&
        std::is_convertible<typename std::remove_reference<T>::type*, U*>::value>
{
};

// is_result

template<class T> struct is_result: std::false_type {};
template<class T, class E> struct is_result< result<T, E> >: std::true_type {};

} // namespace detail

// result | value

template<class T, class E, class U,
    class En = typename std::enable_if<detail::is_value_convertible_to<U, T>::value>::type
>
T operator|( result<T, E> const& r, U&& u )
{
    if( r )
    {
        return *r;
    }
    else
    {
        return std::forward<U>( u );
    }
}

template<class T, class E, class U,
    class En = typename std::enable_if<detail::is_value_convertible_to<U, T>::value>::type
>
T operator|( result<T, E>&& r, U&& u )
{
    if( r )
    {
        return *std::move( r );
    }
    else
    {
        return std::forward<U>( u );
    }
}

// result | nullary-returning-value

template<class T, class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En = typename std::enable_if<detail::is_value_convertible_to<U, T>::value>::type
>
T operator|( result<T, E> const& r, F&& f )
{
    if( r )
    {
        return *r;
    }
    else
    {
        return std::forward<F>( f )();
    }
}

template<class T, class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En = typename std::enable_if<detail::is_value_convertible_to<U, T>::value>::type
>
T operator|( result<T, E>&& r, F&& f )
{
    if( r )
    {
        return *std::move( r );
    }
    else
    {
        return std::forward<F>( f )();
    }
}

// result | nullary-returning-result

template<class T, class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En1 = typename std::enable_if<detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<detail::is_value_convertible_to<T, typename U::value_type>::value>::type
>
U operator|( result<T, E> const& r, F&& f )
{
    if( r )
    {
        return *r;
    }
    else
    {
        return std::forward<F>( f )();
    }
}

template<class T, class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En1 = typename std::enable_if<detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<detail::is_value_convertible_to<T, typename U::value_type>::value>::type
>
U operator|( result<T, E>&& r, F&& f )
{
    if( r )
    {
        return *std::move( r );
    }
    else
    {
        return std::forward<F>( f )();
    }
}

template<class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En1 = typename std::enable_if<detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<std::is_void<typename U::value_type>::value>::type
>
U operator|( result<void, E> const& r, F&& f )
{
    if( r )
    {
        return {};
    }
    else
    {
        return std::forward<F>( f )();
    }
}

template<class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En1 = typename std::enable_if<detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<std::is_void<typename U::value_type>::value>::type
>
U operator|( result<void, E>&& r, F&& f )
{
    if( r )
    {
        return {};
    }
    else
    {
        return std::forward<F>( f )();
    }
}

// operator|=

// result |= value

template<class T, class E, class U,
    class En = typename std::enable_if<detail::is_value_convertible_to<U, T>::value>::type
>
result<T, E>& operator|=( result<T, E>& r, U&& u )
{
    if( !r )
    {
        r = std::forward<U>( u );
    }

    return r;
}

// result |= nullary-returning-value

template<class T, class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En = typename std::enable_if<detail::is_value_convertible_to<U, T>::value>::type
>
result<T, E>& operator|=( result<T, E>& r, F&& f )
{
    if( !r )
    {
        r = std::forward<F>( f )();
    }

    return r;
}

// result |= nullary-returning-result

template<class T, class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En1 = typename std::enable_if<detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<detail::is_value_convertible_to<typename U::value_type, T>::value>::type,
    class En3 = typename std::enable_if<std::is_convertible<typename U::error_type, E>::value>::type
>
result<T, E>& operator|=( result<T, E>& r, F&& f )
{
    if( !r )
    {
        r = std::forward<F>( f )();
    }

    return r;
}

// operator&

// result & unary-returning-value

template<class T, class E, class F,
    class U = decltype( std::declval<F>()( std::declval<T const&>() ) ),
    class En1 = typename std::enable_if<!detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<!std::is_void<U>::value>::type
>
result<U, E> operator&( result<T, E> const& r, F&& f )
{
    if( r.has_error() )
    {
        return r.error();
    }
    else
    {
        return std::forward<F>( f )( *r );
    }
}

template<class T, class E, class F,
    class U = decltype( std::declval<F>()( std::declval<T>() ) ),
    class En1 = typename std::enable_if<!detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<!std::is_void<U>::value>::type
>
result<U, E> operator&( result<T, E>&& r, F&& f )
{
    if( r.has_error() )
    {
        return r.error();
    }
    else
    {
        return std::forward<F>( f )( *std::move( r ) );
    }
}

template<class T, class E, class F,
    class U = decltype( std::declval<F>()( std::declval<T const&>() ) ),
    class En = typename std::enable_if<std::is_void<U>::value>::type
>
result<U, E> operator&( result<T, E> const& r, F&& f )
{
    if( r.has_error() )
    {
        return r.error();
    }
    else
    {
        std::forward<F>( f )( *r );
        return {};
    }
}

template<class T, class E, class F,
    class U = decltype( std::declval<F>()( std::declval<T>() ) ),
    class En = typename std::enable_if<std::is_void<U>::value>::type
>
result<U, E> operator&( result<T, E>&& r, F&& f )
{
    if( r.has_error() )
    {
        return r.error();
    }
    else
    {
        std::forward<F>( f )( *std::move( r ) );
        return {};
    }
}

template<class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En1 = typename std::enable_if<!detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<!std::is_void<U>::value>::type
>
result<U, E> operator&( result<void, E> const& r, F&& f )
{
    if( r.has_error() )
    {
        return r.error();
    }
    else
    {
        return std::forward<F>( f )();
    }
}

template<class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En = typename std::enable_if<std::is_void<U>::value>::type
>
result<U, E> operator&( result<void, E> const& r, F&& f )
{
    if( r.has_error() )
    {
        return r.error();
    }
    else
    {
        std::forward<F>( f )();
        return {};
    }
}

// result & unary-returning-result

template<class T, class E, class F,
    class U = decltype( std::declval<F>()( std::declval<T const&>() ) ),
    class En1 = typename std::enable_if<detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<std::is_convertible<E, typename U::error_type>::value>::type
>
U operator&( result<T, E> const& r, F&& f )
{
    if( r.has_error() )
    {
        return r.error();
    }
    else
    {
        return std::forward<F>( f )( *r );
    }
}

template<class T, class E, class F,
    class U = decltype( std::declval<F>()( std::declval<T>() ) ),
    class En1 = typename std::enable_if<detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<std::is_convertible<E, typename U::error_type>::value>::type
>
U operator&( result<T, E>&& r, F&& f )
{
    if( r.has_error() )
    {
        return r.error();
    }
    else
    {
        return std::forward<F>( f )( *std::move( r ) );
    }
}

template<class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En1 = typename std::enable_if<detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<std::is_convertible<E, typename U::error_type>::value>::type
>
U operator&( result<void, E> const& r, F&& f )
{
    if( r.has_error() )
    {
        return r.error();
    }
    else
    {
        return std::forward<F>( f )();
    }
}

// operator&=

// result &= unary-returning-value

template<class T, class E, class F,
    class U = decltype( std::declval<F>()( std::declval<T>() ) ),
    class En1 = typename std::enable_if<!detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<detail::is_value_convertible_to<U, T>::value>::type
>
result<T, E>& operator&=( result<T, E>& r, F&& f )
{
    if( r )
    {
        r = std::forward<F>( f )( *std::move( r ) );
    }

    return r;
}

template<class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En = typename std::enable_if<!detail::is_result<U>::value>::type
>
result<void, E>& operator&=( result<void, E>& r, F&& f )
{
    if( r )
    {
        std::forward<F>( f )();
    }

    return r;
}

// result &= unary-returning-result

template<class T, class E, class F,
    class U = decltype( std::declval<F>()( std::declval<T>() ) ),
    class En1 = typename std::enable_if<detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<detail::is_value_convertible_to<typename U::value_type, T>::value>::type,
    class En3 = typename std::enable_if<std::is_convertible<typename U::error_type, E>::value>::type
>
result<T, E>& operator&=( result<T, E>& r, F&& f )
{
    if( r )
    {
        r = std::forward<F>( f )( *std::move( r ) );
    }

    return r;
}

template<class E, class F,
    class U = decltype( std::declval<F>()() ),
    class En1 = typename std::enable_if<detail::is_result<U>::value>::type,
    class En2 = typename std::enable_if<std::is_void<typename U::value_type>::value>::type,
    class En3 = typename std::enable_if<std::is_convertible<typename U::error_type, E>::value>::type
>
result<void, E>& operator&=( result<void, E>& r, F&& f )
{
    if( r )
    {
        r = std::forward<F>( f )();
    }

    return r;
}

} // namespace system
} // namespace boost

#endif // #ifndef BOOST_SYSTEM_RESULT_HPP_INCLUDED
