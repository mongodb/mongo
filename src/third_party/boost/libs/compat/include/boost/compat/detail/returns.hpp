#ifndef BOOST_COMPAT_DETAIL_RETURNS_HPP_INCLUDED
#define BOOST_COMPAT_DETAIL_RETURNS_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#define BOOST_COMPAT_RETURNS(...) noexcept(noexcept(__VA_ARGS__)) -> decltype(__VA_ARGS__) { return __VA_ARGS__; }

#endif // BOOST_COMPAT_DETAIL_RETURNS_HPP_INCLUDED
