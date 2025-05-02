//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/conversion.hpp>
#include <boost/locale/encoding.hpp>
#include <boost/locale/generator.hpp>
#include "all_generator.hpp"
#include "api.hpp"
#include <cstring>
#include <locale>
#include <stdexcept>

namespace boost { namespace locale { namespace impl_win {

    class wide_converter final : public converter<wchar_t> {
    public:
        wide_converter(const winlocale& lc, size_t refs = 0) : converter<wchar_t>(refs), lc_(lc) {}
        std::wstring convert(converter_base::conversion_type how,
                             const wchar_t* begin,
                             const wchar_t* end,
                             int flags = 0) const override
        {
            switch(how) {
                case converter_base::upper_case: return towupper_l(begin, end, lc_);
                case converter_base::lower_case: return towlower_l(begin, end, lc_);
                case converter_base::case_folding: return wcsfold(begin, end);
                case converter_base::normalization: return wcsnormalize(static_cast<norm_type>(flags), begin, end);
                case converter_base::title_case: break;
            }
            return std::wstring(begin, end - begin);
        }

    private:
        winlocale lc_;
    };

    template<typename U8Char>
    class utf8_converter final : public converter<U8Char> {
        static_assert(sizeof(U8Char) == sizeof(char), "Not an UTF-8 char type");

    public:
        utf8_converter(const winlocale& lc, size_t refs = 0) : converter<U8Char>(refs), cvt_(lc) {}
        std::basic_string<U8Char> convert(converter_base::conversion_type how,
                                          const U8Char* begin,
                                          const U8Char* end,
                                          int flags = 0) const override
        {
            const std::wstring tmp = conv::utf_to_utf<wchar_t>(begin, end);
            return conv::utf_to_utf<U8Char>(cvt_.convert(how, tmp.c_str(), tmp.c_str() + tmp.size(), flags));
        }

    private:
        wide_converter cvt_;
    };

    std::locale create_convert(const std::locale& in, const winlocale& lc, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return std::locale(in, new utf8_converter<char>(lc));
            case char_facet_t::wchar_f: return std::locale(in, new wide_converter(lc));
#ifdef __cpp_lib_char8_t
            case char_facet_t::char8_f: return std::locale(in, new utf8_converter<char8_t>(lc));
#elif defined(__cpp_char8_t)
            case char_facet_t::char8_f: break;
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: break;
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: break;
#endif
        }
        return in;
    }

}}} // namespace boost::locale::impl_win
