//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2025 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_ENCODING_HPP_INCLUDED
#define BOOST_LOCALE_ENCODING_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <boost/locale/detail/encoding.hpp>
#include <boost/locale/encoding_errors.hpp>
#include <boost/locale/encoding_utf.hpp>
#include <boost/locale/info.hpp>
#include <boost/locale/util/string.hpp>
#include <memory>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost { namespace locale {

    /// \brief Namespace that contains all functions related to character set conversion
    namespace conv {

        /// \defgroup Charset conversion functions
        ///
        /// @{

        /// convert text in range [begin,end) encoded with \a charset to UTF according to policy \a how
        ///
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        BOOST_LOCALE_DECL std::basic_string<CharType>
        to_utf(const char* begin, const char* end, const std::string& charset, method_type how = default_method);

        /// convert UTF text in range [begin,end) to text encoded with \a charset according to policy \a how
        ///
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        BOOST_LOCALE_DECL std::string from_utf(const CharType* begin,
                                               const CharType* end,
                                               const std::string& charset,
                                               method_type how = default_method);

        /// convert \a text encoded with \a charset to UTF according to policy \a how
        ///
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        std::basic_string<CharType>
        to_utf(const std::string& text, const std::string& charset, method_type how = default_method)
        {
            return to_utf<CharType>(text.c_str(), text.c_str() + text.size(), charset, how);
        }

        /// Convert \a text encoded with \a charset to UTF according to policy \a how
        ///
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        std::basic_string<CharType>
        to_utf(const char* text, const std::string& charset, method_type how = default_method)
        {
            return to_utf<CharType>(text, util::str_end(text), charset, how);
        }

        /// convert text in range [begin,end) in locale encoding given by \a loc to UTF according to
        /// policy \a how
        ///
        /// \throws std::bad_cast: \a loc does not have \ref info facet installed
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        std::basic_string<CharType>
        to_utf(const char* begin, const char* end, const std::locale& loc, method_type how = default_method)
        {
            return to_utf<CharType>(begin, end, std::use_facet<info>(loc).encoding(), how);
        }

        /// Convert \a text in locale encoding given by \a loc to UTF according to policy \a how
        ///
        /// \throws std::bad_cast: \a loc does not have \ref info facet installed
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        std::basic_string<CharType>
        to_utf(const std::string& text, const std::locale& loc, method_type how = default_method)
        {
            return to_utf<CharType>(text, std::use_facet<info>(loc).encoding(), how);
        }

        /// Convert \a text in locale encoding given by \a loc to UTF according to policy \a how
        ///
        /// \throws std::bad_cast: \a loc does not have \ref info facet installed
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        std::basic_string<CharType> to_utf(const char* text, const std::locale& loc, method_type how = default_method)
        {
            return to_utf<CharType>(text, std::use_facet<info>(loc).encoding(), how);
        }

        /// convert \a text from UTF to text encoded with \a charset according to policy \a how
        ///
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        std::string
        from_utf(const std::basic_string<CharType>& text, const std::string& charset, method_type how = default_method)
        {
            return from_utf(text.c_str(), text.c_str() + text.size(), charset, how);
        }

        /// Convert \a text from UTF to \a charset according to policy \a how
        ///
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        std::string from_utf(const CharType* text, const std::string& charset, method_type how = default_method)
        {
            return from_utf(text, util::str_end(text), charset, how);
        }

        /// Convert UTF text in range [begin,end) to text in locale encoding given by \a loc according to policy \a how
        ///
        /// \throws std::bad_cast: \a loc does not have \ref info facet installed
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        std::string
        from_utf(const CharType* begin, const CharType* end, const std::locale& loc, method_type how = default_method)
        {
            return from_utf(begin, end, std::use_facet<info>(loc).encoding(), how);
        }

        /// Convert \a text from UTF to locale encoding given by \a loc according to policy \a how
        ///
        /// \throws std::bad_cast: \a loc does not have \ref info facet installed
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        std::string
        from_utf(const std::basic_string<CharType>& text, const std::locale& loc, method_type how = default_method)
        {
            return from_utf(text, std::use_facet<info>(loc).encoding(), how);
        }

        /// Convert \a text from UTF to locale encoding given by \a loc according to policy \a how
        ///
        /// \throws std::bad_cast: \a loc does not have \ref info facet installed
        /// \throws invalid_charset_error: Character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        template<typename CharType>
        std::string from_utf(const CharType* text, const std::locale& loc, method_type how = default_method)
        {
            return from_utf(text, std::use_facet<info>(loc).encoding(), how);
        }

        /// Convert a text in range [begin,end) to \a to_encoding from \a from_encoding according to
        /// policy \a how
        ///
        /// \throws invalid_charset_error: Either character set is not supported
        /// \throws conversion_error: when the conversion fails (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        BOOST_LOCALE_DECL
        std::string between(const char* begin,
                            const char* end,
                            const std::string& to_encoding,
                            const std::string& from_encoding,
                            method_type how = default_method);

        /// Convert \a text to \a to_encoding from \a from_encoding according to
        /// policy \a how
        ///
        /// \throws invalid_charset_error: Either character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        inline std::string between(const char* text,
                                   const std::string& to_encoding,
                                   const std::string& from_encoding,
                                   method_type how = default_method)
        {
            return between(text, util::str_end(text), to_encoding, from_encoding, how);
        }

        /// Convert \a text to \a to_encoding from \a from_encoding according to
        /// policy \a how
        ///
        /// \throws invalid_charset_error: Either character set is not supported
        /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be
        /// encoded or decoded)
        inline std::string between(const std::string& text,
                                   const std::string& to_encoding,
                                   const std::string& from_encoding,
                                   method_type how = default_method)
        {
            return between(text.c_str(), text.c_str() + text.size(), to_encoding, from_encoding, how);
        }

        /// @}

        /// Converter class to decode a narrow string using a local encoding and encode it with UTF
        template<typename CharType>
        class utf_encoder {
            std::unique_ptr<detail::utf_encoder<CharType>> impl_;

        public:
            using char_type = CharType;
            using string_type = std::basic_string<CharType>;

            /// Create an instance to convert text encoded with \a charset to UTF according to policy \a how
            ///
            /// Note: When converting only a single text \ref to_utf is likely faster.
            /// \throws invalid_charset_error: Character set is not supported
            utf_encoder(const std::string& charset, method_type how = default_method) :
                impl_(detail::make_utf_encoder<CharType>(charset, how))
            {}

            /// Convert text in range [begin,end) to UTF
            ///
            /// \throws conversion_error: Conversion failed
            string_type convert(const char* begin, const char* end) const { return impl_->convert(begin, end); }
            /// Convert \a text to UTF
            ///
            /// \throws conversion_error: Conversion failed
            string_type convert(const core::string_view text) const { return impl_->convert(text); }
            /// Convert \a text to UTF
            ///
            /// \throws conversion_error: Conversion failed
            string_type operator()(const core::string_view text) const { return convert(text); }
        };

        /// Converter class to decode an UTF string and encode it using a local encoding
        template<typename CharType>
        class utf_decoder {
            std::unique_ptr<detail::utf_decoder<CharType>> impl_;

        public:
            using char_type = CharType;
            using stringview_type = core::basic_string_view<CharType>;

            /// Create an instance to convert UTF text to text encoded with \a charset according to policy \a how
            ///
            /// Note: When converting only a single text \ref from_utf is likely faster.
            /// \throws invalid_charset_error: Character set is not supported
            utf_decoder(const std::string& charset, method_type how = default_method) :
                impl_(detail::make_utf_decoder<CharType>(charset, how))
            {}

            /// Convert UTF text in range [begin,end) to local encoding
            ///
            /// \throws conversion_error: Conversion failed
            std::string convert(const CharType* begin, const CharType* end) const { return impl_->convert(begin, end); }
            /// Convert \a text from UTF to local encoding
            ///
            /// \throws conversion_error: Conversion failed
            std::string convert(const stringview_type& text) const { return impl_->convert(text); }
            /// Convert \a text from UTF to local encoding
            ///
            /// \throws conversion_error: Conversion failed
            std::string operator()(const stringview_type& text) const { return convert(text); }
        };

        class narrow_converter {
            std::unique_ptr<detail::narrow_converter> impl_;

        public:
            /// Create converter to convert text from \a src_encoding to \a target_encoding according to policy \a how
            ///
            /// \throws invalid_charset_error: Either character set is not supported
            narrow_converter(const std::string& src_encoding,
                             const std::string& target_encoding,
                             method_type how = default_method) :
                impl_(detail::make_narrow_converter(src_encoding, target_encoding, how))
            {}

            /// Convert text in range [begin,end)
            ///
            /// \throws conversion_error: Conversion failed
            std::string convert(const char* begin, const char* end) const { return impl_->convert(begin, end); }
            /// Convert \a text
            ///
            /// \throws conversion_error: Conversion failed
            std::string convert(const core::string_view text) const { return impl_->convert(text); }
            /// Convert \a text
            ///
            /// \throws conversion_error: Conversion failed
            std::string operator()(const core::string_view text) const { return convert(text); }
        };
    } // namespace conv
}}    // namespace boost::locale

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif
