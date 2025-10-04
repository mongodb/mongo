/*
Copyright 2023 Glen Joseph Fernandes
(glenjofe@gmail.com)

Distributed under the Boost Software License, Version 1.0.
(http://www.boost.org/LICENSE_1_0.txt)
*/
#ifndef BOOST_CORE_MAKE_SPAN_HPP
#define BOOST_CORE_MAKE_SPAN_HPP

#include <boost/core/span.hpp>

namespace boost {

template<class I>
inline constexpr span<I>
make_span(I* f, std::size_t c) noexcept
{
    return span<I>(f, c);
}

template<class I>
inline constexpr span<I>
make_span(I* f, I* l) noexcept
{
    return span<I>(f, l);
}

template<class T, std::size_t N>
inline constexpr span<T, N>
make_span(T(&a)[N]) noexcept
{
    return span<T, N>(a);
}

template<class T, std::size_t N>
inline constexpr span<T, N>
make_span(std::array<T, N>& a) noexcept
{
    return span<T, N>(a);
}

template<class T, std::size_t N>
inline constexpr span<const T, N>
make_span(const std::array<T, N>& a) noexcept
{
    return span<const T, N>(a);
}

template<class R>
inline span<typename detail::span_data<R>::type>
make_span(R&& r)
{
    return span<typename detail::span_data<R>::type>(std::forward<R>(r));
}

} /* boost */

#endif
