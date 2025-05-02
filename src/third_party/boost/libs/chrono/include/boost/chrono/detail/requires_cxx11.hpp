#ifndef BOOST_CHRONO_DETAIL_REQUIRES_CXX11_HPP_INCLUDED
#define BOOST_CHRONO_DETAIL_REQUIRES_CXX11_HPP_INCLUDED

// Copyright 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/config.hpp>
#include <boost/config/pragma_message.hpp>

#if defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || \
    defined(BOOST_NO_CXX11_RVALUE_REFERENCES) || \
    defined(BOOST_NO_CXX11_DECLTYPE) || \
    defined(BOOST_NO_CXX11_CONSTEXPR) || \
    defined(BOOST_NO_CXX11_NOEXCEPT) || \
    defined(BOOST_NO_CXX11_HDR_CHRONO) || \
    defined(BOOST_NO_CXX11_HDR_RATIO)

BOOST_PRAGMA_MESSAGE("C++03 support was deprecated in Boost.Chrono 1.82 and was removed in Boost.Chrono 1.84.")

#endif

#endif // #ifndef BOOST_CHRONO_DETAIL_REQUIRES_CXX11_HPP_INCLUDED
