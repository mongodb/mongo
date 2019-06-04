//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_LOCALE_FORMATTER_HPP_INCLUDED
#define BOOST_LOCALE_FORMATTER_HPP_INCLUDED

#include <string>
#include <memory>
#include <boost/cstdint.hpp>
#include <boost/locale/config.hpp>
#include <unicode/locid.h>

namespace boost {
namespace locale {
namespace impl_icu {        

    ///
    /// \brief Special base polymorphic class that is used as a character type independent base for all formatter classes
    ///

    class base_formatter {
    public:
        virtual ~base_formatter()
        {
        }
    };

    ///
    /// \brief A class that is used for formatting numbers, currency and dates/times
    ///
    template<typename CharType>
    class formatter : public base_formatter {
    public:
        typedef CharType char_type;
        typedef std::basic_string<CharType> string_type;

        ///
        /// Format the value and return the number of Unicode code points
        ///
        virtual string_type format(double value,size_t &code_points) const = 0;
        ///
        /// Format the value and return the number of Unicode code points
        ///
        virtual string_type format(int64_t value,size_t &code_points) const = 0;
        ///
        /// Format the value and return the number of Unicode code points
        ///
        virtual string_type format(int32_t value,size_t &code_points) const = 0;

        ///
        /// Parse the string and return the number of used characters. If it returns 0
        /// then parsing failed.
        ///
        virtual size_t parse(string_type const &str,double &value) const = 0;
        ///
        /// Parse the string and return the number of used characters. If it returns 0
        /// then parsing failed.
        ///
        virtual size_t parse(string_type const &str,int64_t &value) const = 0;
        ///
        /// Parse the string and return the number of used characters. If it returns 0
        /// then parsing failed.
        ///
        virtual size_t parse(string_type const &str,int32_t &value) const = 0;

        ///
        /// Get formatter for the current state of ios_base -- flags and locale,
        /// NULL may be returned if an invalid combination of flags is provided or this type
        /// of formatting is not supported by locale. See: create
        ///
        /// Note: formatter is cached. If \a ios is not changed (no flags or locale changed)
        /// the formatter would remain the same. Otherwise it would be rebuild and cached
        /// for future use. It is useful for saving time for generation
        /// of multiple values with same locale.
        ///
        /// For example, this code:
        ///
        /// \code
        ///     std::cout << as::spellout;
        ///     for(int i=1;i<=10;i++)
        ///         std::cout << i <<std::endl;
        /// \endcode
        ///
        /// Would create a new spelling formatter only once.
        ///
        static formatter *create(std::ios_base &ios,icu::Locale const &l,std::string const &enc);

        virtual ~formatter()
        {
        }
    }; // class formatter
    
    ///
    /// Specialization for real implementation
    ///
    template<>
    formatter<char> *formatter<char>::create(std::ios_base &ios,icu::Locale const &l,std::string const &enc);

    ///
    /// Specialization for real implementation
    ///
    template<>
    formatter<wchar_t> *formatter<wchar_t>::create(std::ios_base &ios,icu::Locale const &l,std::string const &e);

    #ifdef BOOST_LOCALE_ENABLE_CHAR16_T
    ///
    /// Specialization for real implementation
    ///
    template<>
    formatter<char16_t> *formatter<char16_t>::create(std::ios_base &ios,icu::Locale const &l,std::string const &e);
    #endif

    #ifdef BOOST_LOCALE_ENABLE_CHAR32_T
    ///
    /// Specialization for real implementation
    ///
    template<>
    formatter<char32_t> *formatter<char32_t>::create(std::ios_base &ios,icu::Locale const &l,std::string const &e);
    #endif

} // namespace impl_icu
} // namespace locale
} // namespace boost



#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4
