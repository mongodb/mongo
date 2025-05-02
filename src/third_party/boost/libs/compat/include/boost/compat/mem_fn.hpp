#ifndef BOOST_COMPAT_MEM_FN_HPP_INCLUDED
#define BOOST_COMPAT_MEM_FN_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/compat/type_traits.hpp>
#include <boost/compat/detail/returns.hpp>
#include <functional>

namespace boost {
namespace compat {

namespace detail {

template<class T, class U, class Td = remove_cvref_t<T>, class Ud = remove_cvref_t<U>>
struct is_same_or_base: std::integral_constant<bool, std::is_same<Td, Ud>::value || std::is_base_of<Td, Ud>::value>
{
};

template<class T> struct is_reference_wrapper_: std::false_type {};
template<class T> struct is_reference_wrapper_< std::reference_wrapper<T> >: std::true_type {};

template<class T> struct is_reference_wrapper: is_reference_wrapper_< remove_cvref_t<T> > {};

template<class M, class T> struct _mfn
{
    M T::* pm_;

    template<class U, class... A, class En = enable_if_t<is_same_or_base<T, U>::value>>
    constexpr auto operator()( U&& u, A&&... a ) const
    BOOST_COMPAT_RETURNS( (std::forward<U>(u).*pm_)( std::forward<A>(a)... ) )

    template<class U, class... A, class = void, class En = enable_if_t< !is_same_or_base<T, U>::value && is_reference_wrapper<U>::value>>
    constexpr auto operator()( U&& u, A&&... a ) const
    BOOST_COMPAT_RETURNS( (u.get().*pm_)( std::forward<A>(a)... ) )

    template<class U, class... A, class = void, class = void, class En = enable_if_t< !is_same_or_base<T, U>::value && !is_reference_wrapper<U>::value>>
    constexpr auto operator()( U&& u, A&&... a ) const
    BOOST_COMPAT_RETURNS( ((*std::forward<U>(u)).*pm_)( std::forward<A>(a)... ) )
};

template<class M, class T> struct _md
{
    M T::* pm_;

    template<class U, class En = enable_if_t<is_same_or_base<T, U>::value>>
    constexpr auto operator()( U&& u ) const
    BOOST_COMPAT_RETURNS( std::forward<U>(u).*pm_ )

    template<class U, class = void, class En = enable_if_t< !is_same_or_base<T, U>::value && is_reference_wrapper<U>::value>>
    constexpr auto operator()( U&& u ) const
    BOOST_COMPAT_RETURNS( u.get().*pm_ )

    template<class U, class = void, class = void, class En = enable_if_t< !is_same_or_base<T, U>::value && !is_reference_wrapper<U>::value>>
    constexpr auto operator()( U&& u ) const
    BOOST_COMPAT_RETURNS( (*std::forward<U>(u)).*pm_ )
};

} // namespace detail

template<class M, class T, class En = enable_if_t< std::is_function<M>::value > >
constexpr auto mem_fn( M T::* pm ) noexcept -> detail::_mfn<M, T>
{
    return detail::_mfn<M, T>{ pm };
}

template<class M, class T, class = void, class En = enable_if_t< !std::is_function<M>::value > >
constexpr auto mem_fn( M T::* pm ) noexcept -> detail::_md<M, T>
{
    return detail::_md<M, T>{ pm };
}

} // namespace compat
} // namespace boost

#endif // BOOST_COMPAT_MEM_FN_HPP_INCLUDED
