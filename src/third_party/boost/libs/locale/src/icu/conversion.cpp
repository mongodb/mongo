//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/conversion.hpp>
#include "all_generator.hpp"
#include "cdata.hpp"
#include "icu_util.hpp"
#include "uconv.hpp"
#include <limits>
#include <unicode/locid.h>
#include <unicode/normlzr.h>
#include <unicode/ucasemap.h>
#include <unicode/ustring.h>
#include <vector>

namespace boost { namespace locale { namespace impl_icu {

    namespace {
        void normalize_string(icu::UnicodeString& str, int flags)
        {
            UErrorCode code = U_ZERO_ERROR;
            UNormalizationMode mode = UNORM_DEFAULT;
            switch(flags) {
                case norm_nfd: mode = UNORM_NFD; break;
                case norm_nfc: mode = UNORM_NFC; break;
                case norm_nfkd: mode = UNORM_NFKD; break;
                case norm_nfkc: mode = UNORM_NFKC; break;
            }
            icu::UnicodeString tmp;
            icu::Normalizer::normalize(str, mode, 0, tmp, code);

            check_and_throw_icu_error(code);

            str = tmp;
        }
    } // namespace

    template<typename CharType>
    class converter_impl : public converter<CharType> {
    public:
        typedef std::basic_string<CharType> string_type;

        converter_impl(const cdata& d) : locale_(d.locale()), encoding_(d.encoding()) {}

        string_type convert(converter_base::conversion_type how,
                            const CharType* begin,
                            const CharType* end,
                            int flags = 0) const override
        {
            icu_std_converter<CharType> cvt(encoding_);
            icu::UnicodeString str = cvt.icu(begin, end);
            using conversion_type = converter_base::conversion_type;
            switch(how) {
                case conversion_type::normalization: normalize_string(str, flags); break;
                case conversion_type::upper_case: str.toUpper(locale_); break;
                case conversion_type::lower_case: str.toLower(locale_); break;
                case conversion_type::title_case: str.toTitle(nullptr, locale_); break;
                case conversion_type::case_folding: str.foldCase(); break;
            }
            return cvt.std(str);
        }

    private:
        icu::Locale locale_;
        std::string encoding_;
    }; // converter_impl

    template<typename T>
    struct get_casemap_size_type;

    template<typename TRes, typename TCaseMap, typename TSize>
    struct get_casemap_size_type<TRes (*)(TCaseMap*, char*, TSize, const char*, TSize, UErrorCode*)> {
        using type = TSize;
    };

    template<typename Func>
    struct is_casemap_func_const;

    template<typename Func>
    struct is_casemap_func_const<Func*> : is_casemap_func_const<Func> {};

    template<typename TRes, typename TCaseMap, typename... TArgs>
    struct is_casemap_func_const<TRes(TCaseMap*, TArgs...)> : std::is_const<TCaseMap> {};

    template<typename U8Char>
    class raii_casemap {
    public:
        static_assert(sizeof(U8Char) == sizeof(char), "Not an UTF-8 char type");
        using string_type = std::basic_string<U8Char>;

        raii_casemap(const raii_casemap&) = delete;
        void operator=(const raii_casemap&) = delete;

        raii_casemap(const std::string& locale_id) : map_(nullptr)
        {
            UErrorCode err = U_ZERO_ERROR;
            map_ = ucasemap_open(locale_id.c_str(), 0, &err);
            check_and_throw_icu_error(err);
            if(!map_)
                throw std::runtime_error("Failed to create UCaseMap"); // LCOV_EXCL_LINE
        }
        ~raii_casemap() { ucasemap_close(map_); }

        template<typename Conv>
        typename std::enable_if<!is_casemap_func_const<Conv>::value, string_type>::type
        convert(Conv func, const U8Char* begin, const U8Char* end)
        {
            return do_convert(func, begin, end);
        }
        template<typename Conv>
        typename std::enable_if<is_casemap_func_const<Conv>::value, string_type>::type
        convert(Conv func, const U8Char* begin, const U8Char* end) const
        {
            return do_convert(func, begin, end);
        }

    private:
        template<typename Conv>
        string_type do_convert(Conv func, const U8Char* begin, const U8Char* end) const
        {
            using size_type = typename get_casemap_size_type<Conv>::type;
            if((end - begin) >= std::numeric_limits<std::ptrdiff_t>::max() / 11)
                throw std::range_error("String to long to be converted by ICU"); // LCOV_EXCL_LINE
            const auto max_converted_size = (end - begin) * 11 / 10 + 1;
            if(max_converted_size >= std::numeric_limits<size_type>::max())
                throw std::range_error("String to long to be converted by ICU"); // LCOV_EXCL_LINE
            std::vector<U8Char> buf(max_converted_size);
            UErrorCode err = U_ZERO_ERROR;
            auto size = func(map_,
                             reinterpret_cast<char*>(buf.data()),
                             static_cast<size_type>(buf.size()),
                             reinterpret_cast<const char*>(begin),
                             static_cast<size_type>(end - begin),
                             &err);
            if(err == U_BUFFER_OVERFLOW_ERROR) {
                err = U_ZERO_ERROR;
                buf.resize(size + 1);
                size = func(map_,
                            reinterpret_cast<char*>(buf.data()),
                            static_cast<size_type>(buf.size()),
                            reinterpret_cast<const char*>(begin),
                            static_cast<size_type>(end - begin),
                            &err);
            }
            check_and_throw_icu_error(err);
            return string_type(buf.data(), size);
        }

    private:
        UCaseMap* map_;
    };

    template<typename U8Char>
    class utf8_converter_impl : public converter<U8Char> {
    public:
        static_assert(sizeof(U8Char) == sizeof(char), "Not an UTF-8 char type");
        utf8_converter_impl(const cdata& d) : locale_id_(d.locale().getName()), map_(locale_id_) {}

        std::basic_string<U8Char> convert(converter_base::conversion_type how,
                                          const U8Char* begin,
                                          const U8Char* end,
                                          int flags = 0) const override
        {
            switch(how) {
                case converter_base::upper_case: return map_.convert(ucasemap_utf8ToUpper, begin, end);
                case converter_base::lower_case: return map_.convert(ucasemap_utf8ToLower, begin, end);
                case converter_base::title_case: {
                    // Non-const method, so need to create a separate map
                    raii_casemap<U8Char> map(locale_id_);
                    return map.convert(ucasemap_utf8ToTitle, begin, end);
                }
                case converter_base::case_folding: return map_.convert(ucasemap_utf8FoldCase, begin, end);
                case converter_base::normalization: {
                    icu_std_converter<U8Char> cvt("UTF-8");
                    icu::UnicodeString str = cvt.icu(begin, end);
                    normalize_string(str, flags);
                    return cvt.std(str);
                }
            }
            return std::basic_string<U8Char>(begin, end - begin); // LCOV_EXCL_LINE
        }

    private:
        std::string locale_id_;
        raii_casemap<U8Char> map_;
    }; // converter_impl

    std::locale create_convert(const std::locale& in, const cdata& cd, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f:
                if(cd.is_utf8())
                    return std::locale(in, new utf8_converter_impl<char>(cd));
                return std::locale(in, new converter_impl<char>(cd));
            case char_facet_t::wchar_f: return std::locale(in, new converter_impl<wchar_t>(cd));
#ifdef __cpp_lib_char8_t
            case char_facet_t::char8_f: return std::locale(in, new utf8_converter_impl<char8_t>(cd));
#elif defined(__cpp_char8_t)
            case char_facet_t::char8_f: break;
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return std::locale(in, new converter_impl<char16_t>(cd));
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return std::locale(in, new converter_impl<char32_t>(cd));
#endif
        }
        return in;
    }

}}} // namespace boost::locale::impl_icu
