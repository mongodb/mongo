#ifndef BOOST_COMPAT_BIND_BACK_HPP_INCLUDED
#define BOOST_COMPAT_BIND_BACK_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/compat/invoke.hpp>
#include <boost/compat/type_traits.hpp>
#include <boost/compat/integer_sequence.hpp>
#include <boost/compat/detail/returns.hpp>
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>
#include <tuple>
#include <utility>

namespace boost {
namespace compat {

namespace detail {

#if BOOST_WORKAROUND(BOOST_MSVC, < 1910)
# pragma warning(push)
# pragma warning(disable: 4100) // 'a': unreferenced formal parameter
#endif

template<class F, class A, class... B, std::size_t... I>
static constexpr auto invoke_bind_back_( F&& f, A&& a, index_sequence<I...>, B&&... b )
    BOOST_COMPAT_RETURNS( compat::invoke( std::forward<F>(f), std::forward<B>(b)..., std::get<I>( std::forward<A>(a) )... ) )

#if BOOST_WORKAROUND(BOOST_MSVC, < 1910)
# pragma warning(pop)
#endif

template<class F, class... A> class bind_back_
{
private:

    F f_;
    std::tuple<A...> a_;

public:

    template<class F2, class... A2>
    constexpr bind_back_( F2&& f2, A2&&... a2 ): f_( std::forward<F2>(f2) ), a_( std::forward<A2>(a2)... ) {}

public:

    template<class... B> BOOST_CXX14_CONSTEXPR auto operator()( B&&... b ) &
        BOOST_COMPAT_RETURNS( detail::invoke_bind_back_( f_, a_, make_index_sequence<sizeof...(A)>(), std::forward<B>(b)... ) )

    template<class... B> constexpr auto operator()( B&&... b ) const &
        BOOST_COMPAT_RETURNS( detail::invoke_bind_back_( f_, a_, make_index_sequence<sizeof...(A)>(), std::forward<B>(b)... ) )

    template<class... B> BOOST_CXX14_CONSTEXPR auto operator()( B&&... b ) &&
        BOOST_COMPAT_RETURNS( detail::invoke_bind_back_( std::move(f_), std::move(a_), make_index_sequence<sizeof...(A)>(), std::forward<B>(b)... ) )

    template<class... B> constexpr auto operator()( B&&... b ) const &&
        BOOST_COMPAT_RETURNS( detail::invoke_bind_back_( std::move(f_), std::move(a_), make_index_sequence<sizeof...(A)>(), std::forward<B>(b)... ) )
};

} // namespace detail

template<class F, class... A> constexpr auto bind_back( F&& f, A&&... a ) -> detail::bind_back_< decay_t<F>, decay_t<A>... >
{
    return { std::forward<F>(f), std::forward<A>(a)... };
}

} // namespace compat
} // namespace boost

#endif // BOOST_COMPAT_BIND_BACK_HPP_INCLUDED
