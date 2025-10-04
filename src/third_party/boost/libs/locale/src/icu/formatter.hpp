//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_FORMATTER_HPP_INCLUDED
#define BOOST_LOCALE_FORMATTER_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <unicode/locid.h>

namespace boost { namespace locale { namespace impl_icu {

    /// \brief Special base polymorphic class that is used as a character type independent base for all formatter
    /// classes
    class base_formatter {
    public:
        virtual ~base_formatter() = default;
    };

    /// \brief A class that is used for formatting numbers, currency and dates/times
    template<typename CharType>
    class formatter : public base_formatter {
    public:
        typedef std::basic_string<CharType> string_type;

        /// Format the value and return the number of Unicode code points
        virtual string_type format(double value, size_t& code_points) const = 0;
        /// Format the value and return the number of Unicode code points
        virtual string_type format(uint64_t value, size_t& code_points) const = 0;
        /// Format the value and return the number of Unicode code points
        virtual string_type format(int64_t value, size_t& code_points) const = 0;
        /// Format the value and return the number of Unicode code points
        virtual string_type format(int32_t value, size_t& code_points) const = 0;

        /// Parse the string and return the number of used characters. If it returns 0
        /// then parsing failed.
        virtual size_t parse(const string_type& str, double& value) const = 0;
        /// Parse the string and return the number of used characters. If it returns 0
        /// then parsing failed.
        virtual size_t parse(const string_type& str, uint64_t& value) const = 0;
        /// Parse the string and return the number of used characters. If it returns 0
        /// then parsing failed.
        virtual size_t parse(const string_type& str, int64_t& value) const = 0;
        /// Parse the string and return the number of used characters. If it returns 0
        /// then parsing failed.
        virtual size_t parse(const string_type& str, int32_t& value) const = 0;

        /// Get formatter for the current state of ios_base -- flags and locale,
        /// NULL may be returned if an invalid combination of flags is provided or this type
        /// of formatting is not supported by locale.
        ///
        /// Note: formatter is cached. If \a ios is not changed (no flags or locale changed)
        /// the formatter would remain the same. Otherwise it would be rebuild and cached
        /// for future use. It is useful for saving time for generation
        /// of multiple values with same locale.
        ///
        /// For example this code will create a new spelling formatter only once:
        ///
        /// \code
        ///     std::cout << as::spellout;
        ///     for(int i=1;i<=10;i++)
        ///         std::cout << i << std::endl;
        /// \endcode
        ///
        ///
        static std::unique_ptr<formatter>
        create(std::ios_base& ios, const icu::Locale& locale, const std::string& encoding);
    }; // class formatter

}}} // namespace boost::locale::impl_icu

#endif
