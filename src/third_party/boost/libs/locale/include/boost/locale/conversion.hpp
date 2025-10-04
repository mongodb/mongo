//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_CONVERTER_HPP_INCLUDED
#define BOOST_LOCALE_CONVERTER_HPP_INCLUDED

#include <boost/locale/detail/facet_id.hpp>
#include <boost/locale/detail/is_supported_char.hpp>
#include <boost/locale/util/string.hpp>
#include <locale>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost { namespace locale {

    /// \defgroup convert Text Conversions
    ///
    ///  This module provides various function for string manipulation like Unicode normalization, case conversion etc.
    /// @{

    /// \brief This class provides base flags for text manipulation. It is used as base for converter facet.
    class converter_base {
    public:
        /// The flag used for facet - the type of operation to perform
        enum conversion_type {
            normalization, ///< Apply Unicode normalization on the text
            upper_case,    ///< Convert text to upper case
            lower_case,    ///< Convert text to lower case
            case_folding,  ///< Fold case in the text
            title_case     ///< Convert text to title case
        };
    };

    /// \brief The facet that implements text manipulation
    ///
    /// It is used to perform text conversion operations defined by \ref converter_base::conversion_type.
    /// It is implemented for supported character types, at least \c char, \c wchar_t
    template<typename Char>
    class BOOST_SYMBOL_VISIBLE converter : public converter_base,
                                           public std::locale::facet,
                                           public detail::facet_id<converter<Char>> {
        BOOST_LOCALE_ASSERT_IS_SUPPORTED(Char);

    public:
        /// Standard constructor
        converter(size_t refs = 0) : std::locale::facet(refs) {}
        /// Convert text in range [\a begin, \a end) according to conversion method \a how. Parameter
        /// \a flags is used for specification of normalization method like nfd, nfc etc.
        virtual std::basic_string<Char>
        convert(conversion_type how, const Char* begin, const Char* end, int flags = 0) const = 0;
    };

    /// The type that defined <a href="http://unicode.org/reports/tr15/#Norm_Forms">normalization form</a>
    enum norm_type {
        norm_nfd,                ///< Canonical decomposition
        norm_nfc,                ///< Canonical decomposition followed by canonical composition
        norm_nfkd,               ///< Compatibility decomposition
        norm_nfkc,               ///< Compatibility decomposition followed by canonical composition.
        norm_default = norm_nfc, ///< Default normalization - canonical decomposition followed by canonical composition
    };

    /// Normalize Unicode string in range [begin,end) according to \ref norm_type "normalization form" \a n
    ///
    /// Note: This function receives only Unicode strings, i.e.: UTF-8, UTF-16 or UTF-32. It does not take
    /// in account the locale encoding, because Unicode decomposition and composition are meaningless outside
    /// of a Unicode character set.
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType> normalize(const CharType* begin,
                                          const CharType* end,
                                          norm_type n = norm_default,
                                          const std::locale& loc = std::locale())
    {
        return std::use_facet<converter<CharType>>(loc).convert(converter_base::normalization, begin, end, n);
    }

    /// Normalize Unicode string \a str according to \ref norm_type "normalization form" \a n
    ///
    /// Note: This function receives only Unicode strings, i.e.: UTF-8, UTF-16 or UTF-32. It does not take
    /// in account the locale encoding, because Unicode decomposition and composition are meaningless outside
    /// of a Unicode character set.
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType> normalize(const std::basic_string<CharType>& str,
                                          norm_type n = norm_default,
                                          const std::locale& loc = std::locale())
    {
        return normalize(str.data(), str.data() + str.size(), n, loc);
    }

    /// Normalize NULL terminated Unicode string \a str according to \ref norm_type "normalization form" \a n
    ///
    /// Note: This function receives only Unicode strings, i.e.: UTF-8, UTF-16 or UTF-32. It does not take
    /// in account the locale encoding, because Unicode decomposition and composition are meaningless outside
    /// of a Unicode character set.
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType>
    normalize(const CharType* str, norm_type n = norm_default, const std::locale& loc = std::locale())
    {
        return normalize(str, util::str_end(str), n, loc);
    }

    ///////////////////////////////////////////////////

    /// Convert a string in range [begin,end) to upper case according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType>
    to_upper(const CharType* begin, const CharType* end, const std::locale& loc = std::locale())
    {
        return std::use_facet<converter<CharType>>(loc).convert(converter_base::upper_case, begin, end);
    }

    /// Convert a string \a str to upper case according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType> to_upper(const std::basic_string<CharType>& str, const std::locale& loc = std::locale())
    {
        return to_upper(str.data(), str.data() + str.size(), loc);
    }

    /// Convert a NULL terminated string \a str to upper case according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType> to_upper(const CharType* str, const std::locale& loc = std::locale())
    {
        return to_upper(str, util::str_end(str), loc);
    }

    ///////////////////////////////////////////////////

    /// Convert a string in range [begin,end) to lower case according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType>
    to_lower(const CharType* begin, const CharType* end, const std::locale& loc = std::locale())
    {
        return std::use_facet<converter<CharType>>(loc).convert(converter_base::lower_case, begin, end);
    }

    /// Convert a string \a str to lower case according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType> to_lower(const std::basic_string<CharType>& str, const std::locale& loc = std::locale())
    {
        return to_lower(str.data(), str.data() + str.size(), loc);
    }

    /// Convert a NULL terminated string \a str to lower case according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType> to_lower(const CharType* str, const std::locale& loc = std::locale())
    {
        return to_lower(str, util::str_end(str), loc);
    }

    ///////////////////////////////////////////////////

    /// Convert a string in range [begin,end) to title case according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType>
    to_title(const CharType* begin, const CharType* end, const std::locale& loc = std::locale())
    {
        return std::use_facet<converter<CharType>>(loc).convert(converter_base::title_case, begin, end);
    }

    /// Convert a string \a str to title case according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType> to_title(const std::basic_string<CharType>& str, const std::locale& loc = std::locale())
    {
        return to_title(str.data(), str.data() + str.size(), loc);
    }

    /// Convert a NULL terminated string \a str to title case according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType> to_title(const CharType* str, const std::locale& loc = std::locale())
    {
        return to_title(str, util::str_end(str), loc);
    }

    ///////////////////////////////////////////////////

    /// Fold case of a string in range [begin,end) according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType>
    fold_case(const CharType* begin, const CharType* end, const std::locale& loc = std::locale())
    {
        return std::use_facet<converter<CharType>>(loc).convert(converter_base::case_folding, begin, end);
    }

    /// Fold case of a string \a str according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType> fold_case(const std::basic_string<CharType>& str,
                                          const std::locale& loc = std::locale())
    {
        return fold_case(str.data(), str.data() + str.size(), loc);
    }

    /// Fold case of a NULL terminated string \a str according to locale \a loc
    ///
    /// \throws std::bad_cast: \a loc does not have \ref converter facet installed
    template<typename CharType>
    std::basic_string<CharType> fold_case(const CharType* str, const std::locale& loc = std::locale())
    {
        return fold_case(str, util::str_end(str), loc);
    }

    ///@}
}} // namespace boost::locale

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

/// \example conversions.cpp
///
/// Example of using various text conversion functions.
///
/// \example wconversions.cpp
///
/// Example of using various text conversion functions with wide strings.

#endif
