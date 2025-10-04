//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2023-2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_UTIL_LOCALE_DATA_HPP
#define BOOST_LOCALE_UTIL_LOCALE_DATA_HPP

#include <boost/locale/config.hpp>
#include <string>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4251)
#endif

namespace boost { namespace locale { namespace util {

    /// Holder and parser for locale names/identifiers
    class BOOST_LOCALE_DECL locale_data {
        std::string language_;
        std::string script_;
        std::string country_;
        std::string encoding_;
        std::string variant_;
        bool utf8_;

    public:
        /// Default to C locale with US-ASCII encoding
        locale_data();
        /// Construct from the parsed locale \see \ref parse
        ///
        /// \throws std::invalid_argument: parsing failed
        explicit locale_data(const std::string& locale_name);

        /// Return language (usually 2 lowercase letters, i.e. ISO-639 or 'C')
        const std::string& language() const { return language_; }
        /// Return the ISO-15924 abbreviation script code if present
        const std::string& script() const { return script_; }
        /// Return country (usually 2 uppercase letters, i.e. ISO-3166)
        const std::string& country() const { return country_; }
        /// Return encoding/codeset, e.g. ISO8859-1 or UTF-8
        const std::string& encoding() const { return encoding_; }
        /// Set encoding, will be made uppercase by default as-if it was parsed
        /// Returns \c *this for chaining
        locale_data& encoding(std::string new_encoding, bool uppercase = true);
        /// Return variant/modifier, e.g. euro or stroke
        const std::string& variant() const { return variant_; }
        /// Return iff the encoding is UTF-8
        bool is_utf8() const { return utf8_; }

        /// Parse a locale identifier of the form `[language[_script][_territory][.codeset][@modifier]]`
        ///
        /// Allows a dash as the delimiter: `[language-territory]`
        /// Return true if the identifier is valid:
        ///   - `language` is given and consists of ASCII letters
        ///   - `script` is only considered if it consists of exactly 4 ASCII letters
        ///   - `territory`, if given, consists of ASCII letters (usually ISO-3166)
        ///   - Any field started by a delimiter (`_`, `-`, `.`, `@`) is not empty
        /// Otherwise parsing is aborted. Valid values already parsed stay set, other are defaulted.
        bool parse(const std::string& locale_name);

        /// Get a representation in the form `[language[_territory][.codeset][@modifier]]`
        /// codeset is omitted if it is US-ASCII
        std::string to_string() const;

    private:
        void reset();
        bool parse_from_lang(const std::string& input);
        bool parse_from_script(const std::string& input);
        bool parse_from_country(const std::string& input);
        bool parse_from_encoding(const std::string& input);
        bool parse_from_variant(const std::string& input);
    };

}}} // namespace boost::locale::util

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif
#endif
