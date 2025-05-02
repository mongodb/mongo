#ifndef BOOST_BIND_MEM_FN_HPP_INCLUDED
#define BOOST_BIND_MEM_FN_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//
// mem_fn.hpp - a generalization of std::mem_fun[_ref]
//
// Copyright 2001-2005, 2024 Peter Dimov
// Copyright 2001 David Abrahams
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/bind/mem_fn.html for documentation.
//

#include <boost/get_pointer.hpp>
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>
#include <type_traits>

namespace boost
{

namespace _mfi
{

template<class T> struct remove_cvref: std::remove_cv< typename std::remove_reference<T>::type >
{
};

template<class Pm, class R, class T, class... A> class mf
{
public:

    typedef R result_type;

private:

    Pm pm_;

public:

    mf( Pm pm ): pm_( pm ) {}

    template<class U,
        class Ud = typename _mfi::remove_cvref<U>::type,
        class En = typename std::enable_if<
            std::is_same<T, Ud>::value || std::is_base_of<T, Ud>::value
        >::type
    >

    R operator()( U&& u, A... a ) const
    {
        return (std::forward<U>( u ).*pm_)( std::forward<A>( a )... );
    }

    template<class U,
        class Ud = typename _mfi::remove_cvref<U>::type,
        class E1 = void,
        class En = typename std::enable_if<
            !(std::is_same<T, Ud>::value || std::is_base_of<T, Ud>::value)
        >::type
    >

    R operator()( U&& u, A... a ) const
    {
        return (get_pointer( std::forward<U>( u ) )->*pm_)( std::forward<A>( a )... );
    }

    bool operator==( mf const & rhs ) const
    {
        return pm_ == rhs.pm_;
    }

    bool operator!=( mf const & rhs ) const
    {
        return pm_ != rhs.pm_;
    }
};

} // namespace _mfi

//

template<class R, class T, class... A>
auto mem_fn( R (T::*pmf) (A...) ) -> _mfi::mf<decltype(pmf), R, T, A...>
{
    return pmf;
}

template<class R, class T, class... A>
auto mem_fn( R (T::*pmf) (A...) const ) -> _mfi::mf<decltype(pmf), R, T, A...>
{
    return pmf;
}

#if defined( __cpp_noexcept_function_type ) || defined( _NOEXCEPT_TYPES_SUPPORTED )

template<class R, class T, class... A>
auto mem_fn( R (T::*pmf) (A...) noexcept ) -> _mfi::mf<decltype(pmf), R, T, A...>
{
    return pmf;
}

template<class R, class T, class... A>
auto mem_fn( R (T::*pmf) (A...) const noexcept ) -> _mfi::mf<decltype(pmf), R, T, A...>
{
    return pmf;
}

#endif // #if defined( __cpp_noexcept_function_type ) || defined( _NOEXCEPT_TYPES_SUPPORTED )

#if defined(BOOST_MEM_FN_ENABLE_CDECL) && !defined(_M_X64)

template<class R, class T, class... A>
auto mem_fn( R (__cdecl T::*pmf) (A...) ) -> _mfi::mf<decltype(pmf), R, T, A...>
{
    return pmf;
}

template<class R, class T, class... A>
auto mem_fn( R (__cdecl T::*pmf) (A...) const ) -> _mfi::mf<decltype(pmf), R, T, A...>
{
    return pmf;
}

#endif // #if defined(BOOST_MEM_FN_ENABLE_CDECL) && !defined(_M_X64)

#if defined(BOOST_MEM_FN_ENABLE_STDCALL) && !defined(_M_X64)

template<class R, class T, class... A>
auto mem_fn( R (__stdcall T::*pmf) (A...) ) -> _mfi::mf<decltype(pmf), R, T, A...>
{
    return pmf;
}

template<class R, class T, class... A>
auto mem_fn( R (__stdcall T::*pmf) (A...) const ) -> _mfi::mf<decltype(pmf), R, T, A...>
{
    return pmf;
}

#endif // #if defined(BOOST_MEM_FN_ENABLE_STDCALL) && !defined(_M_X64)

#if defined(BOOST_MEM_FN_ENABLE_FASTCALL) && !defined(_M_X64)

template<class R, class T, class... A>
auto mem_fn( R (__fastcall T::*pmf) (A...) ) -> _mfi::mf<decltype(pmf), R, T, A...>
{
    return pmf;
}

template<class R, class T, class... A>
auto mem_fn( R (__fastcall T::*pmf) (A...) const ) -> _mfi::mf<decltype(pmf), R, T, A...>
{
    return pmf;
}

#endif // #if defined(BOOST_MEM_FN_ENABLE_FASTCALL) && !defined(_M_X64)

// data member support

namespace _mfi
{

template<class R, class T> class dm
{
public:

    typedef R const & result_type;
    typedef T const * argument_type;

private:

    typedef R (T::*Pm);
    Pm pm_;

public:

    dm( Pm pm ): pm_( pm ) {}

    template<class U,
        class Ud = typename _mfi::remove_cvref<U>::type,
        class En = typename std::enable_if<
            std::is_same<T, Ud>::value || std::is_base_of<T, Ud>::value
        >::type
    >

    auto operator()( U&& u ) const -> decltype( std::forward<U>( u ).*pm_ )
    {
        return std::forward<U>( u ).*pm_;
    }

    template<class U,
        class Ud = typename _mfi::remove_cvref<U>::type,
        class E1 = void,
        class En = typename std::enable_if<
            !(std::is_same<T, Ud>::value || std::is_base_of<T, Ud>::value)
        >::type
    >

    auto operator()( U&& u ) const -> decltype( get_pointer( std::forward<U>( u ) )->*pm_ )
    {
        return get_pointer( std::forward<U>( u ) )->*pm_;
    }

#if BOOST_WORKAROUND(BOOST_MSVC, < 1910)

    template<class U>
    R& operator()( U* u ) const
    {
        return u->*pm_;
    }

    template<class U>
    R const& operator()( U const* u ) const
    {
        return u->*pm_;
    }

#endif

    bool operator==( dm const & rhs ) const
    {
        return pm_ == rhs.pm_;
    }

    bool operator!=( dm const & rhs ) const
    {
        return pm_ != rhs.pm_;
    }
};

} // namespace _mfi

template<class R, class T,
    class E = typename std::enable_if< !std::is_function<R>::value >::type
>
_mfi::dm<R, T> mem_fn( R T::*pm )
{
    return pm;
}

} // namespace boost

#endif // #ifndef BOOST_BIND_MEM_FN_HPP_INCLUDED
