//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/conversion.hpp>
#include <boost/locale/encoding.hpp>
#include <boost/locale/generator.hpp>
#include <locale>
#include <stdexcept>
#include <vector>

#include "all_generator.hpp"

namespace boost { namespace locale { namespace impl_std {

    template<typename CharType>
    class std_converter : public converter<CharType> {
    public:
        typedef std::basic_string<CharType> string_type;
        typedef std::ctype<CharType> ctype_type;

        std_converter(const std::string& locale_name) :
            base_(std::locale::classic(), new std::ctype_byname<CharType>(locale_name))
        {}
        string_type convert(converter_base::conversion_type how,
                            const CharType* begin,
                            const CharType* end,
                            int /*flags*/ = 0) const override
        {
            switch(how) {
                case converter_base::upper_case:
                case converter_base::lower_case:
                case converter_base::case_folding: {
                    const ctype_type& ct = std::use_facet<ctype_type>(base_);
                    size_t len = end - begin;
                    std::vector<CharType> res(len + 1, 0);
                    CharType* lbegin = res.data();
                    std::copy(begin, end, lbegin);
                    if(how == converter_base::upper_case)
                        ct.toupper(lbegin, lbegin + len);
                    else
                        ct.tolower(lbegin, lbegin + len);
                    return string_type(lbegin, len);
                }
                case converter_base::normalization:
                case converter_base::title_case: break;
            }
            return string_type(begin, end - begin);
        }

    private:
        std::locale base_;
    };

    template<typename U8Char>
    class utf8_converter : public converter<U8Char> {
    public:
        typedef std::basic_string<U8Char> string_type;
        typedef std::ctype<wchar_t> wctype_type;

        utf8_converter(const std::string& locale_name) :
            base_(std::locale::classic(), new std::ctype_byname<wchar_t>(locale_name))
        {}
        string_type convert(converter_base::conversion_type how,
                            const U8Char* begin,
                            const U8Char* end,
                            int /*flags*/ = 0) const override
        {
            using conversion_type = converter_base::conversion_type;
            switch(how) {
                case conversion_type::upper_case:
                case conversion_type::lower_case:
                case conversion_type::case_folding: {
                    std::wstring tmp = conv::utf_to_utf<wchar_t>(begin, end);
                    const wctype_type& ct = std::use_facet<wctype_type>(base_);
                    wchar_t* lbegin = &tmp.front();
                    const size_t len = tmp.size();
                    if(how == conversion_type::upper_case)
                        ct.toupper(lbegin, lbegin + len);
                    else
                        ct.tolower(lbegin, lbegin + len);
                    return conv::utf_to_utf<U8Char>(lbegin, lbegin + len);
                }
                case conversion_type::title_case:
                case conversion_type::normalization: break;
            }
            return string_type(begin, end - begin);
        }

    private:
        std::locale base_;
    };

    std::locale
    create_convert(const std::locale& in, const std::string& locale_name, char_facet_t type, utf8_support utf)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f:
                if(utf != utf8_support::none)
                    return std::locale(in, new utf8_converter<char>(locale_name));
                else
                    return std::locale(in, new std_converter<char>(locale_name));
            case char_facet_t::wchar_f: return std::locale(in, new std_converter<wchar_t>(locale_name));
#ifdef __cpp_lib_char8_t
            case char_facet_t::char8_f: return std::locale(in, new utf8_converter<char8_t>(locale_name));
#elif defined(__cpp_char8_t)
            case char_facet_t::char8_f: break;
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return std::locale(in, new std_converter<char16_t>(locale_name));
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return std::locale(in, new std_converter<char32_t>(locale_name));
#endif
        }
        return in;
    }

}}} // namespace boost::locale::impl_std
