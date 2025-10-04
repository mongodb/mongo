//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_INFO_HPP_INCLUDED
#define BOOST_LOCALE_INFO_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <boost/locale/detail/facet_id.hpp>
#include <locale>
#include <string>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost { namespace locale {

    /// \brief a facet that holds general information about locale
    ///
    /// This facet should be always created in order to make all Boost.Locale functions work
    class BOOST_SYMBOL_VISIBLE info : public std::locale::facet, public detail::facet_id<info> {
    public:
        /// String information about the locale
        enum string_property {
            language_property, ///< ISO 639 language id
            country_property,  ///< ISO 3166 country id
            variant_property,  ///< Variant for locale
            encoding_property, ///< encoding name
            name_property      ///< locale name
        };

        /// Integer information about locale
        enum integer_property {
            utf8_property ///< Non zero value if uses UTF-8 encoding
        };

        /// Standard facet's constructor
        info(size_t refs = 0) : std::locale::facet(refs) {}
        /// Get language name
        std::string language() const { return get_string_property(language_property); }
        /// Get country name
        std::string country() const { return get_string_property(country_property); }
        /// Get locale variant
        std::string variant() const { return get_string_property(variant_property); }
        /// Get encoding
        std::string encoding() const { return get_string_property(encoding_property); }

        /// Get the name of the locale, like en_US.UTF-8
        std::string name() const { return get_string_property(name_property); }

        /// True if the underlying encoding is UTF-8 (for char streams and strings)
        bool utf8() const { return get_integer_property(utf8_property) != 0; }

    protected:
        /// Get string property by its id \a v
        virtual std::string get_string_property(string_property v) const = 0;
        /// Get integer property by its id \a v
        virtual int get_integer_property(integer_property v) const = 0;
    };

}} // namespace boost::locale

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif
