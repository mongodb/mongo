// Copyright Alexander Nasonov & Paul A. Bristow 2006.

// Use, modification and distribution are subject to the
// Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_DETAIL_LCAST_PRECISION_HPP_INCLUDED
#define BOOST_DETAIL_LCAST_PRECISION_HPP_INCLUDED

#include <climits>
#include <ios>
#include <limits>

#include <boost/config.hpp>

namespace boost { namespace detail {

// Calculate an argument to pass to std::ios_base::precision from
// lexical_cast.
template<class T>
struct lcast_precision
{
    using limits = std::numeric_limits<T>;

    static constexpr bool use_default_precision =
            !limits::is_specialized || limits::is_exact
        ;

    static constexpr bool is_specialized_bin =
            !use_default_precision &&
            limits::radix == 2 && limits::digits > 0
        ;

    static constexpr bool is_specialized_dec =
            !use_default_precision &&
            limits::radix == 10 && limits::digits10 > 0
        ;

    static constexpr std::streamsize streamsize_max =
            (std::numeric_limits<std::streamsize>::max)()
        ;

    static constexpr unsigned int precision_dec = limits::digits10 + 1U;

    static_assert(!is_specialized_dec ||
            precision_dec <= streamsize_max + 0UL
        , "");

    static constexpr unsigned long precision_bin =
            2UL + limits::digits * 30103UL / 100000UL
        ;

    static_assert(!is_specialized_bin ||
            (limits::digits + 0UL < ULONG_MAX / 30103UL &&
            precision_bin > limits::digits10 + 0UL &&
            precision_bin <= streamsize_max + 0UL)
        , "");

    static constexpr std::streamsize value =
            is_specialized_bin ? precision_bin
                               : is_specialized_dec ? precision_dec : 6
        ;
};

template<class T>
inline void lcast_set_precision(std::ios_base& stream, T*)
{
    stream.precision(lcast_precision<T>::value);
}

template<class Source, class Target>
inline void lcast_set_precision(std::ios_base& stream, Source*, Target*)
{
    std::streamsize const s = lcast_precision<Source>::value;
    std::streamsize const t = lcast_precision<Target*>::value;
    stream.precision(s > t ? s : t);
}

}}

#endif //  BOOST_DETAIL_LCAST_PRECISION_HPP_INCLUDED

