//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_GENERATOR_HPP
#define BOOST_LOCALE_GENERATOR_HPP

#include <boost/locale/hold_ptr.hpp>
#include <cstdint>
#include <locale>
#include <memory>
#include <string>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost {

///
/// \brief This is the main namespace that encloses all localization classes
///
namespace locale {

    class localization_backend;
    class localization_backend_manager;

    /// Type that specifies the character type that locales can be generated for
    ///
    /// Supports bitwise OR and bitwise AND (the latter returning if the type is set)
    enum class char_facet_t : uint32_t {
        nochar = 0,       ///< Unspecified character category for character independent facets
        char_f = 1 << 0,  ///< 8-bit character facets
        wchar_f = 1 << 1, ///< wide character facets
#ifdef __cpp_char8_t
        char8_f = 1 << 2, ///< C++20 char8_t facets
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
        char16_f = 1 << 3, ///< C++11 char16_t facets
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
        char32_f = 1 << 4, ///< C++11 char32_t facets
#endif
    };
    typedef BOOST_DEPRECATED("Use char_facet_t") char_facet_t character_facet_type;

    /// First facet specific for character type
    constexpr char_facet_t character_facet_first = char_facet_t::char_f;
    /// Last facet specific for character type
    constexpr char_facet_t character_facet_last =
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
      char_facet_t::char32_f;
#elif defined BOOST_LOCALE_ENABLE_CHAR16_T
      char_facet_t::char16_f;
#elif defined __cpp_char8_t
      char_facet_t::char8_f;
#else
      char_facet_t::wchar_f;
#endif
    /// Special mask -- generate all
    constexpr char_facet_t all_characters = char_facet_t(0xFFFFFFFFu);

    /// Type used for more fine grained generation of facets
    ///
    /// Supports bitwise OR and bitwise AND (the latter returning if the type is set)
    enum class category_t : uint32_t {
        convert = 1 << 0,      ///< Generate conversion facets
        collation = 1 << 1,    ///< Generate collation facets
        formatting = 1 << 2,   ///< Generate numbers, currency, date-time formatting facets
        parsing = 1 << 3,      ///< Generate numbers, currency, date-time formatting facets
        message = 1 << 4,      ///< Generate message facets
        codepage = 1 << 5,     ///< Generate character set conversion facets (derived from std::codecvt)
        boundary = 1 << 6,     ///< Generate boundary analysis facet
        calendar = 1 << 16,    ///< Generate boundary analysis facet
        information = 1 << 17, ///< Generate general locale information facet
    };
    typedef BOOST_DEPRECATED("Use category_t") category_t locale_category_type;

    /// First facet specific for character
    constexpr category_t per_character_facet_first = category_t::convert;
    /// Last facet specific for character
    constexpr category_t per_character_facet_last = category_t::boundary;
    /// First character independent facet
    constexpr category_t non_character_facet_first = category_t::calendar;
    /// Last character independent facet
    constexpr category_t non_character_facet_last = category_t::information;
    /// First category facet
    constexpr category_t category_first = category_t::convert;
    /// Last category facet
    constexpr category_t category_last = category_t::information;
    /// Generate all of them
    constexpr category_t all_categories = category_t(0xFFFFFFFFu);

    /// \brief the major class used for locale generation
    ///
    /// This class is used for specification of all parameters required for locale generation and
    /// caching. This class const member functions are thread safe if locale class implementation is thread safe.
    class BOOST_LOCALE_DECL generator {
    public:
        /// Create new generator using global localization_backend_manager
        generator();
        /// Create new generator using specific localization_backend_manager
        generator(const localization_backend_manager&);

        ~generator();

        /// Set types of facets that should be generated, default all
        void categories(category_t cats);
        /// Get types of facets that should be generated, default all
        category_t categories() const;

        /// Set the characters type for which the facets should be generated, default all supported
        void characters(char_facet_t chars);
        /// Get the characters type for which the facets should be generated, default all supported
        char_facet_t characters() const;

        /// Add a new domain of messages that would be generated. It should be set in order to enable
        /// messages support.
        ///
        /// Messages domain has following format: "name" or "name/encoding"
        /// where name is the base name of the "mo" file where the catalog is stored
        /// without ".mo" extension. For example for file \c /usr/share/locale/he/LC_MESSAGES/blog.mo
        /// it would be \c blog.
        ///
        /// You can optionally specify the encoding of the keys in the sources by adding "/encoding_name"
        /// For example blog/cp1255.
        ///
        /// If not defined all keys are assumed to be UTF-8 encoded.
        ///
        /// \note When you select a domain for the program using dgettext or message API, you
        /// do not specify the encoding part. So for example if the provided
        /// domain name was "blog/windows-1255" then for translation
        /// you should use dgettext("blog","Hello")
        void add_messages_domain(const std::string& domain);

        /// Set default message domain. If this member was not called, the first added messages domain is used.
        /// If the domain \a domain is not added yet it is added.
        void set_default_messages_domain(const std::string& domain);

        /// Remove all added domains from the list
        void clear_domains();

        /// Add a search path where dictionaries are looked in.
        ///
        /// \note
        ///
        /// - Under the Windows platform the path is treated as a path in the locale's encoding so
        ///   if you create locale "en_US.windows-1251" then path would be treated as cp1255,
        ///   and if it is en_US.UTF-8 it is treated as UTF-8. File name is always opened with
        ///   a wide file name as wide file names are the native file name on Windows.
        ///
        /// - Under POSIX platforms all paths passed as-is regardless of encoding as narrow
        ///   encodings are the native encodings for POSIX platforms.
        ///
        void add_messages_path(const std::string& path);

        /// Remove all added paths
        void clear_paths();

        /// Remove all cached locales
        void clear_cache();

        /// Turn locale caching ON
        void locale_cache_enabled(bool on);

        /// Get locale cache option
        bool locale_cache_enabled() const;

        /// Check if by default ANSI encoding is selected or UTF-8 onces. The default is false.
        bool use_ansi_encoding() const;

        /// Select ANSI encodings as default system encoding rather then UTF-8 by default
        /// under Windows.
        ///
        /// The default is the most portable and most powerful encoding, UTF-8, but the user
        /// can select "system" one if dealing with legacy applications
        void use_ansi_encoding(bool enc);

        /// Generate a locale with id \a id
        std::locale generate(const std::string& id) const;
        /// Generate a locale with id \a id. Use \a base as a locale to which all facets are added,
        /// instead of std::locale::classic().
        std::locale generate(const std::locale& base, const std::string& id) const;
        /// Shortcut to generate(id)
        std::locale operator()(const std::string& id) const { return generate(id); }

    private:
        void set_all_options(localization_backend& backend, const std::string& id) const;

        struct data;
        hold_ptr<data> d;
    };

    constexpr char_facet_t operator|(const char_facet_t lhs, const char_facet_t rhs)
    {
        return char_facet_t(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }
    constexpr char_facet_t operator^(const char_facet_t lhs, const char_facet_t rhs)
    {
        return char_facet_t(static_cast<uint32_t>(lhs) ^ static_cast<uint32_t>(rhs));
    }
    constexpr bool operator&(const char_facet_t lhs, const char_facet_t rhs)
    {
        return (static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs)) != 0u;
    }
    // Prefix increment: Return the next value
    BOOST_CXX14_CONSTEXPR inline char_facet_t& operator++(char_facet_t& v)
    {
        return v = char_facet_t(static_cast<uint32_t>(v) ? static_cast<uint32_t>(v) << 1 : 1);
    }

    constexpr category_t operator|(const category_t lhs, const category_t rhs)
    {
        return category_t(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }
    constexpr category_t operator^(const category_t lhs, const category_t rhs)
    {
        return category_t(static_cast<uint32_t>(lhs) ^ static_cast<uint32_t>(rhs));
    }
    constexpr bool operator&(const category_t lhs, const category_t rhs)
    {
        return (static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs)) != 0u;
    }
    // Prefix increment: Return the next value
    BOOST_CXX14_CONSTEXPR inline category_t& operator++(category_t& v)
    {
        return v = category_t(static_cast<uint32_t>(v) << 1);
    }
} // namespace locale
} // namespace boost
#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif
