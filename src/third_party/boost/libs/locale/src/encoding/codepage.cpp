//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2025 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/encoding.hpp>
#include "../util/make_std_unique.hpp"

#if BOOST_LOCALE_USE_WIN32_API
#    define BOOST_LOCALE_WITH_WCONV
#endif
#ifdef BOOST_LOCALE_WITH_ICONV
#    include "iconv_converter.hpp"
#endif
#ifdef BOOST_LOCALE_WITH_ICU
#    include "uconv_converter.hpp"
#endif
#ifdef BOOST_LOCALE_WITH_WCONV
#    include "wconv_converter.hpp"
#endif

namespace boost { namespace locale { namespace conv {

    std::string between(const char* begin,
                        const char* end,
                        const std::string& to_charset,
                        const std::string& from_charset,
                        method_type how)
    {
#ifdef BOOST_LOCALE_WITH_ICONV
        {
            impl::iconv_between cvt;
            if(cvt.open(to_charset, from_charset, how))
                return cvt.convert(begin, end);
        }
#endif
#ifdef BOOST_LOCALE_WITH_ICU
        {
            impl::uconv_between cvt;
            if(cvt.open(to_charset, from_charset, how))
                return cvt.convert(begin, end);
        }
#endif
#ifdef BOOST_LOCALE_WITH_WCONV
        {
            impl::wconv_between cvt;
            if(cvt.open(to_charset, from_charset, how))
                return cvt.convert(begin, end);
        }
#endif
        throw invalid_charset_error(std::string(to_charset) + " or " + from_charset);
    }

    template<typename CharType>
    std::basic_string<CharType> to_utf(const char* begin, const char* end, const std::string& charset, method_type how)
    {
#ifdef BOOST_LOCALE_WITH_ICONV
        {
            impl::iconv_to_utf<CharType> cvt;
            if(cvt.open(charset, how))
                return cvt.convert(begin, end);
        }
#endif
#ifdef BOOST_LOCALE_WITH_ICU
        {
            impl::uconv_to_utf<CharType> cvt;
            if(cvt.open(charset, how))
                return cvt.convert(begin, end);
        }
#endif
#ifdef BOOST_LOCALE_WITH_WCONV
        {
            impl::wconv_to_utf<CharType> cvt;
            if(cvt.open(charset, how))
                return cvt.convert(begin, end);
        }
#endif
        throw invalid_charset_error(charset);
    }

    template<typename CharType>
    std::string from_utf(const CharType* begin, const CharType* end, const std::string& charset, method_type how)
    {
#ifdef BOOST_LOCALE_WITH_ICONV
        {
            impl::iconv_from_utf<CharType> cvt;
            if(cvt.open(charset, how))
                return cvt.convert(begin, end);
        }
#endif
#ifdef BOOST_LOCALE_WITH_ICU
        {
            impl::uconv_from_utf<CharType> cvt;
            if(cvt.open(charset, how))
                return cvt.convert(begin, end);
        }
#endif
#ifdef BOOST_LOCALE_WITH_WCONV
        {
            impl::wconv_from_utf<CharType> cvt;
            if(cvt.open(charset, how))
                return cvt.convert(begin, end);
        }
#endif
        throw invalid_charset_error(charset);
    }

    namespace detail {
        template<class T>
        static std::unique_ptr<T> move_to_ptr(T& c)
        {
            return make_std_unique<T>(std::move(c));
        }

        template<typename Char>
        std::unique_ptr<utf_encoder<Char>>
        make_utf_encoder(const std::string& charset, method_type how, conv_backend impl)
        {
#ifdef BOOST_LOCALE_WITH_ICONV
            if(impl == conv_backend::Default || impl == conv_backend::IConv) {
                impl::iconv_to_utf<Char> cvt;
                if(cvt.open(charset, how))
                    return move_to_ptr(cvt);
            }
#endif
#ifdef BOOST_LOCALE_WITH_ICU
            if(impl == conv_backend::Default || impl == conv_backend::ICU) {
                impl::uconv_to_utf<Char> cvt;
                if(cvt.open(charset, how))
                    return move_to_ptr(cvt);
            }
#endif
#ifdef BOOST_LOCALE_WITH_WCONV
            if(impl == conv_backend::Default || impl == conv_backend::WinAPI) {
                impl::wconv_to_utf<Char> cvt;
                if(cvt.open(charset, how))
                    return move_to_ptr(cvt);
            }
#endif
            throw invalid_charset_error(charset);
        }

        template<typename Char>
        std::unique_ptr<utf_decoder<Char>>
        make_utf_decoder(const std::string& charset, method_type how, conv_backend impl)
        {
#ifdef BOOST_LOCALE_WITH_ICONV
            if(impl == conv_backend::Default || impl == conv_backend::IConv) {
                impl::iconv_from_utf<Char> cvt;
                if(cvt.open(charset, how))
                    return move_to_ptr(cvt);
            }
#endif
#ifdef BOOST_LOCALE_WITH_ICU
            if(impl == conv_backend::Default || impl == conv_backend::ICU) {
                impl::uconv_from_utf<Char> cvt;
                if(cvt.open(charset, how))
                    return move_to_ptr(cvt);
            }
#endif
#ifdef BOOST_LOCALE_WITH_WCONV
            if(impl == conv_backend::Default || impl == conv_backend::WinAPI) {
                impl::wconv_from_utf<Char> cvt;
                if(cvt.open(charset, how))
                    return move_to_ptr(cvt);
            }
#endif
            throw invalid_charset_error(charset);
        }
        std::unique_ptr<narrow_converter> make_narrow_converter(const std::string& src_encoding,
                                                                const std::string& target_encoding,
                                                                method_type how,
                                                                conv_backend impl)
        {
#ifdef BOOST_LOCALE_WITH_ICONV
            if(impl == conv_backend::Default || impl == conv_backend::IConv) {
                impl::iconv_between cvt;
                if(cvt.open(target_encoding, src_encoding, how))
                    return move_to_ptr(cvt);
            }
#endif
#ifdef BOOST_LOCALE_WITH_ICU
            if(impl == conv_backend::Default || impl == conv_backend::ICU) {
                impl::uconv_between cvt;
                if(cvt.open(target_encoding, src_encoding, how))
                    return move_to_ptr(cvt);
            }
#endif
#ifdef BOOST_LOCALE_WITH_WCONV
            if(impl == conv_backend::Default || impl == conv_backend::WinAPI) {
                impl::wconv_between cvt;
                if(cvt.open(target_encoding, src_encoding, how))
                    return move_to_ptr(cvt);
            }
#endif
            throw invalid_charset_error(std::string(src_encoding) + " or " + target_encoding);
        }
    } // namespace detail

#define BOOST_LOCALE_INSTANTIATE(CHARTYPE)                                                              \
    namespace detail {                                                                                  \
        template class charset_converter<char, CHARTYPE>;                                               \
        template BOOST_LOCALE_DECL std::unique_ptr<utf_encoder<CHARTYPE>>                               \
        make_utf_encoder(const std::string& charset, method_type how, conv_backend impl);               \
        template BOOST_LOCALE_DECL std::unique_ptr<utf_decoder<CHARTYPE>>                               \
        make_utf_decoder(const std::string& charset, method_type how, conv_backend impl);               \
    }                                                                                                   \
    template BOOST_LOCALE_DECL std::basic_string<CHARTYPE> to_utf<CHARTYPE>(const char* begin,          \
                                                                            const char* end,            \
                                                                            const std::string& charset, \
                                                                            method_type how);           \
    template BOOST_LOCALE_DECL std::string from_utf<CHARTYPE>(const CHARTYPE* begin,                    \
                                                              const CHARTYPE* end,                      \
                                                              const std::string& charset,               \
                                                              method_type how)
#define BOOST_LOCALE_INSTANTIATE_NO_CHAR(CHARTYPE)        \
    BOOST_LOCALE_INSTANTIATE(CHARTYPE);                   \
    namespace detail {                                    \
        template class charset_converter<CHARTYPE, char>; \
    }

    BOOST_LOCALE_INSTANTIATE(char);
    BOOST_LOCALE_INSTANTIATE_NO_CHAR(wchar_t);

#ifdef __cpp_lib_char8_t
    BOOST_LOCALE_INSTANTIATE_NO_CHAR(char8_t);
#endif

#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
    BOOST_LOCALE_INSTANTIATE_NO_CHAR(char16_t);
#endif

#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
    BOOST_LOCALE_INSTANTIATE_NO_CHAR(char32_t);
#endif

}}} // namespace boost::locale::conv
