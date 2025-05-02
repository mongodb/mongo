//
// Copyright (c) 2022-2025 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_DETAIL_ENCODING_HPP_INCLUDED
#define BOOST_LOCALE_DETAIL_ENCODING_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <boost/locale/encoding_errors.hpp>
#include <boost/core/detail/string_view.hpp>
#include <memory>
#include <string>

/// \cond INTERNAL
namespace boost { namespace locale { namespace conv { namespace detail {
    template<typename CharIn, typename CharOut>
    class BOOST_SYMBOL_VISIBLE charset_converter {
    public:
        using char_out_type = CharOut;
        using char_in_type = CharIn;
        using string_type = std::basic_string<CharOut>;

        virtual ~charset_converter() = default;
        virtual string_type convert(const CharIn* begin, const CharIn* end) = 0;
        string_type convert(const core::basic_string_view<CharIn> text)
        {
            return convert(text.data(), text.data() + text.length());
        }
    };

    using narrow_converter = charset_converter<char, char>;

    template<typename CharType>
    using utf_encoder = charset_converter<char, CharType>;

    template<typename CharType>
    using utf_decoder = charset_converter<CharType, char>;

    enum class conv_backend { Default, IConv, ICU, WinAPI };

    template<typename Char>
    BOOST_LOCALE_DECL std::unique_ptr<utf_encoder<Char>>
    make_utf_encoder(const std::string& charset, method_type how, conv_backend impl = conv_backend::Default);
    template<typename Char>
    BOOST_LOCALE_DECL std::unique_ptr<utf_decoder<Char>>
    make_utf_decoder(const std::string& charset, method_type how, conv_backend impl = conv_backend::Default);
    BOOST_LOCALE_DECL std::unique_ptr<narrow_converter>
    make_narrow_converter(const std::string& src_encoding,
                          const std::string& target_encoding,
                          method_type how,
                          conv_backend impl = conv_backend::Default);
}}}} // namespace boost::locale::conv::detail

/// \endcond

#endif
