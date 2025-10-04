//
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_DETAIL_IS_SUPPORTED_CHAR_HPP_INCLUDED
#define BOOST_LOCALE_DETAIL_IS_SUPPORTED_CHAR_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <type_traits>

/// \cond INTERNAL
namespace boost { namespace locale { namespace detail {
    /// Trait, returns true iff the argument is a supported character type
    template<typename Char>
    struct is_supported_char : std::false_type {};

    template<>
    struct is_supported_char<char> : std::true_type {};
    template<>
    struct is_supported_char<wchar_t> : std::true_type {};
#ifdef __cpp_char8_t
    template<>
    struct is_supported_char<char8_t> : std::true_type {};
#endif

#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
    template<>
    struct is_supported_char<char16_t> : std::true_type {};
#endif

#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
    template<>
    struct is_supported_char<char32_t> : std::true_type {};
#endif

    template<typename Char>
    using enable_if_is_supported_char = typename std::enable_if<is_supported_char<Char>::value>::type;

}}} // namespace boost::locale::detail

#define BOOST_LOCALE_ASSERT_IS_SUPPORTED(Char) \
    static_assert(boost::locale::detail::is_supported_char<Char>::value, "Unsupported Char type")

/// \endcond

#endif
