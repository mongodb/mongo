//
// Copyright (c) 2024-2025 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_UTIL_NUMERIC_CONVERSIONS_HPP
#define BOOST_LOCALE_IMPL_UTIL_NUMERIC_CONVERSIONS_HPP

#include <boost/locale/config.hpp>
#include <boost/assert.hpp>
#include <boost/charconv/from_chars.hpp>
#include <boost/core/detail/string_view.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#ifdef BOOST_LOCALE_WITH_ICU
#    include <unicode/fmtable.h>
#endif

namespace boost { namespace locale { namespace util {
    namespace {

        // Create lookup table where: powers_of_10[i] == 10**i
        constexpr uint64_t pow10(unsigned exponent)
        {
            return (exponent == 0) ? 1 : pow10(exponent - 1) * 10u;
        }
        template<bool condition, std::size_t Length>
        using array_if_true = typename std::enable_if<condition, std::array<uint64_t, Length>>::type;

        template<std::size_t Length, typename... Values>
        constexpr array_if_true<sizeof...(Values) == Length, Length> make_powers_of_10(Values... values)
        {
            return {{values...}};
        }
        template<std::size_t Length, typename... Values>
        constexpr array_if_true<sizeof...(Values) < Length, Length> make_powers_of_10(Values... values)
        {
            return make_powers_of_10<Length>(values..., pow10(sizeof...(Values)));
        }
        constexpr auto powers_of_10 = make_powers_of_10<std::numeric_limits<uint64_t>::digits10 + 1>();
#ifndef BOOST_NO_CXX14_CONSTEXPR
        static_assert(powers_of_10[0] == 1u, "!");
        static_assert(powers_of_10[1] == 10u, "!");
        static_assert(powers_of_10[5] == 100000u, "!");
#endif
    } // namespace

    template<typename Integer>
    bool try_to_int(core::string_view s, Integer& value)
    {
        if(s.size() >= 2 && s[0] == '+') {
            if(s[1] == '-') // "+-" is not allowed, invalid "+<number>" is detected by parser
                return false;
            s.remove_prefix(1);
        }
        const auto res = boost::charconv::from_chars(s, value);
        return res && res.ptr == (s.data() + s.size());
    }

    /// Parse a string in scientific format to an integer.
    /// In particular the "E notation" is used.
    /// I.e. "\d.\d+E\d+", e.g. 5.12E3 == 5120; 5E2 == 500; 2E+1 == 20)
    /// Additionally plain integers are recognized.
    template<typename Integer>
    bool try_scientific_to_int(const core::string_view s, Integer& value)
    {
        static_assert(std::is_integral<Integer>::value && std::is_unsigned<Integer>::value,
                      "Must be an  unsigned integer");
        if(s.size() < 3) // At least: iEj for E notation
            return try_to_int(s, value);
        if(s[0] == '-')
            return false;
        constexpr auto maxDigits = std::numeric_limits<Integer>::digits10 + 1;

        const auto expPos = s.find('E', 1);
        if(expPos == core::string_view::npos)
            return (s[1] != '.') && try_to_int(s, value); // Shortcut: Regular integer
        uint8_t exponent;                                 // Negative exponent would be a fractional
        if(BOOST_UNLIKELY(!try_to_int(s.substr(expPos + 1), exponent)))
            return false;

        core::string_view significant = s.substr(0, expPos);
        Integer significant_value;
        if(s[1] == '.') {
            const auto numSignificantDigits = significant.size() - 1u; // Exclude dot
            const auto numDigits = exponent + 1u;                      // E0 -> 1 digit
            if(BOOST_UNLIKELY(numDigits < numSignificantDigits))
                return false; // Fractional
            else if(BOOST_UNLIKELY(numDigits > maxDigits))
                return false; // Too large
            // Factor to get from the fractional number to an integer
            BOOST_ASSERT(numSignificantDigits - 1u < powers_of_10.size());
            const auto factor = static_cast<Integer>(powers_of_10[numSignificantDigits - 1]);
            exponent = static_cast<uint8_t>(numDigits - numSignificantDigits);

            const unsigned firstDigit = significant[0] - '0';
            if(firstDigit > 9u)
                return false; // Not a digit
            if(numSignificantDigits == maxDigits) {
                const auto maxFirstDigit = std::numeric_limits<Integer>::max() / powers_of_10[maxDigits - 1];
                if(firstDigit > maxFirstDigit)
                    return false;
            }
            significant.remove_prefix(2);
            if(BOOST_UNLIKELY(!try_to_int(significant, significant_value)))
                return false;
            // firstDigit * factor + significant_value <= max
            if(static_cast<Integer>(firstDigit) > (std::numeric_limits<Integer>::max() - significant_value) / factor)
                return false;
            significant_value += static_cast<Integer>(firstDigit * factor);
        } else if(BOOST_UNLIKELY(significant.size() + exponent > maxDigits))
            return false;
        else if(BOOST_UNLIKELY(!try_to_int(significant, significant_value)))
            return false;
        // Add zeros if necessary
        if(exponent > 0u) {
            BOOST_ASSERT(exponent < powers_of_10.size());
            const auto factor = static_cast<Integer>(powers_of_10[exponent]);
            if(significant_value > std::numeric_limits<Integer>::max() / factor)
                return false;
            value = significant_value * factor;
        } else
            value = significant_value;
        return true;
    }

#ifdef BOOST_LOCALE_WITH_ICU
    template<typename Integer>
    bool try_parse_icu(icu::Formattable& fmt, Integer& value)
    {
        if(!fmt.isNumeric())
            return false;
        // Get value as a decimal number and parse that
        UErrorCode err = U_ZERO_ERROR;
        const auto decimals = fmt.getDecimalNumber(err);
        if(U_FAILURE(err))
            return false; // Memory error LCOV_EXCL_LINE
        const core::string_view s(decimals.data(), decimals.length());
        return try_scientific_to_int(s, value);
    }
#endif
}}} // namespace boost::locale::util

#endif
