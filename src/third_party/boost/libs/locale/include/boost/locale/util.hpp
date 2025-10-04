//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_UTIL_HPP
#define BOOST_LOCALE_UTIL_HPP

#include <boost/locale/generator.hpp>
#include <boost/locale/utf.hpp>
#include <boost/assert.hpp>
#include <cstdint>
#include <locale>
#include <memory>
#include <typeinfo>

namespace boost { namespace locale {
    /// \brief This namespace provides various utility function useful for Boost.Locale's backends
    /// implementations
    namespace util {

        /// \brief Return default system locale name in POSIX format.
        ///
        /// This function tries to detect the locale using LC_ALL, LC_CTYPE and LANG environment
        /// variables in this order and if all of them are unset, on POSIX platforms it returns "C".
        /// On Windows additionally to the above environment variables, this function
        /// tries to create the locale name from ISO-639 and ISO-3166 country codes defined
        /// for the users default locale.
        /// If \a use_utf8_on_windows is true it sets the encoding to UTF-8,
        /// otherwise, if the system locale supports ANSI codepages it defines the ANSI encoding, e.g. windows-1252,
        /// otherwise (if ANSI codepage is not available) it uses UTF-8 encoding.
        BOOST_LOCALE_DECL
        std::string get_system_locale(bool use_utf8_on_windows = false);

        /// \brief Installs information facet to locale \a in based on locale name \a name
        ///
        /// This function installs boost::locale::info facet into the locale \a in and returns
        /// newly created locale.
        ///
        /// Note: all information is based only on parsing of string \a name;
        ///
        /// The name has following format: language[_COUNTRY][.encoding][\@variant]
        /// Where language is ISO-639 language code like "en" or "ru", COUNTRY is ISO-3166
        /// country identifier like "US" or "RU". the Encoding is a character set name
        /// like UTF-8 or ISO-8859-1. Variant is backend specific variant like \c euro or
        /// calendar=hebrew.
        ///
        /// If some parameters are missing they are specified as blanks, default encoding
        /// is assumed to be US-ASCII and missing language is assumed to be "C"
        BOOST_LOCALE_DECL
        std::locale create_info(const std::locale& in, const std::string& name);

        /// \brief This class represent a simple stateless converter from UCS-4 and to UCS-4 for
        ///  each single code point
        ///
        /// This class is used for creation of std::codecvt facet for converting utf-16/utf-32 encoding
        /// to encoding supported by this converter
        ///
        /// Please note, this converter should be fully stateless. Fully stateless means it should
        /// never assume that it is called in any specific order on the text. Even if the
        /// encoding itself seems to be stateless like windows-1255 or shift-jis, some
        /// encoders (most notably iconv) can actually compose several code-point into one or
        /// decompose them in case composite characters are found. So be very careful when implementing
        /// these converters for certain character set.
        class BOOST_LOCALE_DECL base_converter {
        public:
            /// This value should be returned when an illegal input sequence or code-point is observed:
            /// For example if a UCS-32 code-point is in the range reserved for UTF-16 surrogates
            /// or an invalid UTF-8 sequence is found
            static constexpr utf::code_point illegal = utf::illegal;

            /// This value is returned in following cases: An incomplete input sequence was found or
            /// insufficient output buffer was provided so complete output could not be written.
            static constexpr utf::code_point incomplete = utf::incomplete;

            virtual ~base_converter();

            /// Return the maximal length that one Unicode code-point can be converted to, for example
            /// for UTF-8 it is 4, for Shift-JIS it is 2 and ISO-8859-1 is 1
            virtual int max_len() const { return 1; }

            /// Returns true if calling the functions from_unicode, to_unicode, and max_len is thread safe.
            ///
            /// Rule of thumb: if this class' implementation uses simple tables that are unchanged
            /// or is purely algorithmic like UTF-8 - so it does not share any mutable bit for
            /// independent to_unicode, from_unicode calls, you may set it to true, otherwise,
            /// for example if you use iconv_t descriptor or UConverter as conversion object return false,
            /// and this object will be cloned for each use.
            virtual bool is_thread_safe() const { return false; }

            /// Create a polymorphic copy of this object, usually called only if is_thread_safe() return false
            virtual base_converter* clone() const
            {
                BOOST_ASSERT(typeid(*this) == typeid(base_converter));
                return new base_converter();
            }

            /// Convert a single character starting at begin and ending at most at end to Unicode code-point.
            ///
            /// if valid input sequence found in [\a begin,\a code_point_end) such as \a begin < \a code_point_end && \a
            /// code_point_end <= \a end it is converted to its Unicode code point equivalent, \a begin is set to \a
            /// code_point_end
            ///
            /// if incomplete input sequence found in [\a begin,\a end), i.e. there my be such \a code_point_end that \a
            /// code_point_end > \a end and [\a begin, \a code_point_end) would be valid input sequence, then \a
            /// incomplete is returned begin stays unchanged, for example for UTF-8 conversion a *begin = 0xc2, \a begin
            /// +1 = \a end is such situation.
            ///
            /// if invalid input sequence found, i.e. there is a sequence [\a begin, \a code_point_end) such as \a
            /// code_point_end <= \a end that is illegal for this encoding, \a illegal is returned and begin stays
            /// unchanged. For example if *begin = 0xFF and begin < end for UTF-8, then \a illegal is returned.
            virtual utf::code_point to_unicode(const char*& begin, const char* end)
            {
                if(begin == end)
                    return incomplete; // LCOV_EXCL_LINE
                unsigned char cp = *begin;
                if(cp <= 0x7F) {
                    begin++;
                    return cp;
                }
                return illegal;
            }

            /// Convert a single code-point \a u into encoding and store it in [begin,end) range.
            ///
            /// If u is invalid Unicode code-point, or it can not be mapped correctly to represented character set,
            /// \a illegal should be returned
            ///
            /// If u can be converted to a sequence of bytes c1, ... , cN (1<= N <= max_len() ) then
            ///
            /// -# If end - begin >= N, c1, ... cN are written starting at begin and N is returned
            /// -# If end - begin < N, incomplete is returned, it is unspecified what would be
            ///    stored in bytes in range [begin,end)
            virtual utf::len_or_error from_unicode(utf::code_point u, char* begin, const char* end)
            {
                if(begin == end)
                    return incomplete; // LCOV_EXCL_LINE
                if(u >= 0x80)
                    return illegal;
                *begin = static_cast<char>(u);
                return 1;
            }
        };

        /// This function creates a \a base_converter that can be used for conversion between UTF-8 and
        /// Unicode code points
        BOOST_LOCALE_DECL std::unique_ptr<base_converter> create_utf8_converter();

        BOOST_DEPRECATED("This function is deprecated, use 'create_utf8_converter()'")
        inline std::unique_ptr<base_converter> create_utf8_converter_unique_ptr()
        {
            return create_utf8_converter();
        }

        /// This function creates a \a base_converter that can be used for conversion between single byte
        /// character encodings like ISO-8859-1, koi8-r, windows-1255 and Unicode code points,
        ///
        /// If \a encoding is not supported, empty pointer is returned.
        /// So you should check whether the returned pointer is valid/non-NULL
        BOOST_LOCALE_DECL std::unique_ptr<base_converter> create_simple_converter(const std::string& encoding);

        BOOST_DEPRECATED("This function is deprecated, use 'create_simple_converter()'")
        inline std::unique_ptr<base_converter> create_simple_converter_unique_ptr(const std::string& encoding)
        {
            return create_simple_converter(encoding);
        }

        /// Install codecvt facet into locale \a in and return new locale that is based on \a in and uses new
        /// facet.
        ///
        /// codecvt facet would convert between narrow and wide/char16_t/char32_t encodings using \a cvt converter.
        /// If \a cvt is null pointer, always failure conversion would be used that fails on every first input or
        /// output.
        ///
        /// Note: the codecvt facet handles both UTF-16 and UTF-32 wide encodings, it knows to break and join
        /// Unicode code-points above 0xFFFF to and from surrogate pairs correctly. \a cvt should be unaware
        /// of wide encoding type
        BOOST_LOCALE_DECL
        std::locale create_codecvt(const std::locale& in, std::unique_ptr<base_converter> cvt, char_facet_t type);

        BOOST_DEPRECATED("This function is deprecated, use 'create_codecvt()'")
        inline std::locale create_codecvt_from_pointer(const std::locale& in, base_converter* cvt, char_facet_t type)
        {
            return create_codecvt(in, std::unique_ptr<base_converter>(cvt), type);
        }

        BOOST_DEPRECATED("This function is deprecated, use 'create_utf8_converter()'")
        BOOST_LOCALE_DECL base_converter* create_utf8_converter_new_ptr();

        BOOST_DEPRECATED("This function is deprecated, use 'create_simple_converter()'")
        BOOST_LOCALE_DECL base_converter* create_simple_converter_new_ptr(const std::string& encoding);

        /// Install utf8 codecvt to UTF-16 or UTF-32 into locale \a in and return
        /// new locale that is based on \a in and uses new facet.
        BOOST_LOCALE_DECL
        std::locale create_utf8_codecvt(const std::locale& in, char_facet_t type);

        /// This function installs codecvt that can be used for conversion between single byte
        /// character encodings like ISO-8859-1, koi8-r, windows-1255 and Unicode code points,
        ///
        /// \throws boost::locale::conv::invalid_charset_error: Character set is not supported or isn't a single
        /// byte character set
        BOOST_LOCALE_DECL
        std::locale create_simple_codecvt(const std::locale& in, const std::string& encoding, char_facet_t type);
    } // namespace util
}}    // namespace boost::locale

#endif
