//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/conversion.hpp>
#include <boost/locale/encoding.hpp>
#include <boost/locale/generator.hpp>
#include "../util/encoding.hpp"
#include <cctype>
#include <cstring>
#include <langinfo.h>
#include <locale>
#include <stdexcept>
#include <wctype.h>
#if defined(__FreeBSD__)
#    include <xlocale.h>
#endif

#include "all_generator.hpp"

namespace boost { namespace locale { namespace impl_posix {

    template<typename CharType>
    struct case_traits;

    template<>
    struct case_traits<char> {
        static char lower(char c, locale_t lc) { return tolower_l(c, lc); }
        static char upper(char c, locale_t lc) { return toupper_l(c, lc); }
    };

    template<>
    struct case_traits<wchar_t> {
        static wchar_t lower(wchar_t c, locale_t lc) { return towlower_l(c, lc); }
        static wchar_t upper(wchar_t c, locale_t lc) { return towupper_l(c, lc); }
    };

    template<typename CharType>
    class std_converter : public converter<CharType> {
    public:
        typedef std::basic_string<CharType> string_type;
        typedef std::ctype<CharType> ctype_type;
        std_converter(std::shared_ptr<locale_t> lc, size_t refs = 0) : converter<CharType>(refs), lc_(std::move(lc)) {}
        string_type convert(converter_base::conversion_type how,
                            const CharType* begin,
                            const CharType* end,
                            int /*flags*/ = 0) const override
        {
            switch(how) {
                case converter_base::upper_case: {
                    string_type res;
                    res.reserve(end - begin);
                    while(begin != end)
                        res += case_traits<CharType>::upper(*begin++, *lc_);
                    return res;
                }
                case converter_base::lower_case:
                case converter_base::case_folding: {
                    string_type res;
                    res.reserve(end - begin);
                    while(begin != end)
                        res += case_traits<CharType>::lower(*begin++, *lc_);
                    return res;
                }
                case converter_base::normalization:
                case converter_base::title_case: break;
            }
            return string_type(begin, end - begin);
        }

    private:
        std::shared_ptr<locale_t> lc_;
    };

    template<typename U8Char>
    class utf8_converter : public converter<U8Char> {
    public:
        static_assert(sizeof(U8Char) == sizeof(char), "Not an UTF-8 char type");

        utf8_converter(std::shared_ptr<locale_t> lc, size_t refs = 0) : converter<U8Char>(refs), lc_(std::move(lc)) {}
        std::basic_string<U8Char> convert(converter_base::conversion_type how,
                                          const U8Char* begin,
                                          const U8Char* end,
                                          int /*flags*/ = 0) const override
        {
            using conversion_type = converter_base::conversion_type;
            switch(how) {
                case conversion_type::upper_case: {
                    const std::wstring tmp = conv::utf_to_utf<wchar_t>(begin, end);
                    std::wstring wres;
                    wres.reserve(tmp.size());
                    for(const wchar_t c : tmp)
                        wres += towupper_l(c, *lc_);
                    return conv::utf_to_utf<U8Char>(wres);
                }

                case conversion_type::lower_case:
                case conversion_type::case_folding: {
                    const std::wstring tmp = conv::utf_to_utf<wchar_t>(begin, end);
                    std::wstring wres;
                    wres.reserve(tmp.size());
                    for(const wchar_t c : tmp)
                        wres += towlower_l(c, *lc_);
                    return conv::utf_to_utf<U8Char>(wres);
                }
                case conversion_type::normalization:
                case conversion_type::title_case: break;
            }
            return std::basic_string<U8Char>(begin, end - begin);
        }

    private:
        std::shared_ptr<locale_t> lc_;
    };

    std::locale create_convert(const std::locale& in, std::shared_ptr<locale_t> lc, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: {
                if(util::normalize_encoding(nl_langinfo_l(CODESET, *lc)) == "utf8")
                    return std::locale(in, new utf8_converter<char>(std::move(lc)));
                return std::locale(in, new std_converter<char>(std::move(lc)));
            }
            case char_facet_t::wchar_f: return std::locale(in, new std_converter<wchar_t>(std::move(lc)));
#ifdef __cpp_lib_char8_t
            case char_facet_t::char8_f: return std::locale(in, new utf8_converter<char8_t>(std::move(lc)));
#elif defined(__cpp_char8_t)
            case char_facet_t::char8_f: break;
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return std::locale(in, new std_converter<char16_t>(std::move(lc)));
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return std::locale(in, new std_converter<char32_t>(std::move(lc)));
#endif
        }
        return in;
    }

}}} // namespace boost::locale::impl_posix
