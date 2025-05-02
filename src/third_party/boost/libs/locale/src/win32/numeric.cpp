//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include "../util/numeric.hpp"
#include <boost/locale/encoding.hpp>
#include <boost/locale/formatting.hpp>
#include <boost/locale/generator.hpp>
#include "all_generator.hpp"
#include "api.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <ios>
#include <locale>
#include <sstream>
#include <string>
#include <wctype.h>

namespace boost { namespace locale { namespace impl_win {
    namespace {
        std::ostreambuf_iterator<wchar_t> write_it(std::ostreambuf_iterator<wchar_t> out, const std::wstring& s)
        {
            return std::copy(s.begin(), s.end(), out);
        }

        std::ostreambuf_iterator<char> write_it(std::ostreambuf_iterator<char> out, const std::wstring& s)
        {
            const std::string tmp = conv::utf_to_utf<char>(s);
            return std::copy(tmp.begin(), tmp.end(), out);
        }
    } // namespace

    template<typename CharType>
    class num_format : public util::base_num_format<CharType> {
    public:
        typedef typename std::num_put<CharType>::iter_type iter_type;
        typedef std::basic_string<CharType> string_type;

        num_format(const winlocale& lc, size_t refs = 0) : util::base_num_format<CharType>(refs), lc_(lc) {}

    private:
        iter_type do_format_currency(bool /*intl*/,
                                     iter_type out,
                                     std::ios_base& ios,
                                     CharType fill,
                                     long double val) const override
        {
            if(lc_.is_c()) {
                std::locale loc = ios.getloc();
                int digits = std::use_facet<std::moneypunct<CharType>>(loc).frac_digits();
                while(digits > 0) {
                    val *= 10;
                    digits--;
                }
                std::ios_base::fmtflags f = ios.flags();
                ios.flags(f | std::ios_base::showbase);
                out = std::use_facet<std::money_put<CharType>>(loc).put(out, false, ios, fill, val);
                ios.flags(f);
                return out;
            } else {
                std::wstring cur = wcsfmon_l(static_cast<double>(val), lc_);
                return write_it(out, cur);
            }
        }

    private:
        winlocale lc_;

    }; // num_format

    template<typename CharType>
    class time_put_win : public std::time_put<CharType> {
    public:
        time_put_win(const winlocale& lc, size_t refs = 0) : std::time_put<CharType>(refs), lc_(lc) {}

        typedef typename std::time_put<CharType>::iter_type iter_type;
        typedef std::basic_string<CharType> string_type;

        iter_type do_put(iter_type out,
                         std::ios_base& /*ios*/,
                         CharType /*fill*/,
                         const std::tm* tm,
                         char format,
                         char /*modifier*/) const override
        {
            return write_it(out, wcsftime_l(format, tm, lc_));
        }

    private:
        winlocale lc_;
    };

    template<typename CharType>
    class num_punct_win : public std::numpunct<CharType> {
    public:
        typedef std::basic_string<CharType> string_type;
        num_punct_win(const winlocale& lc, size_t refs = 0) : std::numpunct<CharType>(refs)
        {
            numeric_info np = wcsnumformat_l(lc);

            BOOST_LOCALE_START_CONST_CONDITION
            if(sizeof(CharType) == 1 && np.thousands_sep == L"\xA0")
                np.thousands_sep = L" ";
            BOOST_LOCALE_END_CONST_CONDITION

            to_str(np.thousands_sep, thousands_sep_);
            to_str(np.decimal_point, decimal_point_);
            grouping_ = np.grouping;
            if(thousands_sep_.size() > 1)
                grouping_ = std::string();
            if(decimal_point_.size() > 1)
                decimal_point_ = CharType('.');
        }

        void to_str(std::wstring& s1, std::wstring& s2) { s2.swap(s1); }

        void to_str(std::wstring& s1, std::string& s2) { s2 = conv::utf_to_utf<char>(s1); }
        CharType do_decimal_point() const override { return *decimal_point_.c_str(); }
        CharType do_thousands_sep() const override { return *thousands_sep_.c_str(); }
        std::string do_grouping() const override { return grouping_; }
        string_type do_truename() const override
        {
            static const char t[] = "true";
            return string_type(t, t + sizeof(t) - 1);
        }
        string_type do_falsename() const override
        {
            static const char t[] = "false";
            return string_type(t, t + sizeof(t) - 1);
        }

    private:
        string_type decimal_point_;
        string_type thousands_sep_;
        std::string grouping_;
    };

    template<typename CharType>
    std::locale create_formatting_impl(const std::locale& in, const winlocale& lc)
    {
        if(lc.is_c()) {
            std::locale tmp(in, new std::numpunct_byname<CharType>("C"));
            tmp = std::locale(tmp, new std::time_put_byname<CharType>("C"));
            tmp = std::locale(tmp, new num_format<CharType>(lc));
            return tmp;
        } else {
            std::locale tmp(in, new num_punct_win<CharType>(lc));
            tmp = std::locale(tmp, new time_put_win<CharType>(lc));
            tmp = std::locale(tmp, new num_format<CharType>(lc));
            return tmp;
        }
    }

    template<typename CharType>
    std::locale create_parsing_impl(const std::locale& in, const winlocale& lc)
    {
        std::numpunct<CharType>* np = 0;
        if(lc.is_c())
            np = new std::numpunct_byname<CharType>("C");
        else
            np = new num_punct_win<CharType>(lc);
        std::locale tmp(in, np);
        tmp = std::locale(tmp, new util::base_num_parse<CharType>());
        return tmp;
    }

    std::locale create_formatting(const std::locale& in, const winlocale& lc, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return create_formatting_impl<char>(in, lc);
            case char_facet_t::wchar_f: return create_formatting_impl<wchar_t>(in, lc);
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // std-facet not available (yet)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return create_formatting_impl<char16_t>(in, lc);
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return create_formatting_impl<char32_t>(in, lc);
#endif
        }
        return in;
    }

    std::locale create_parsing(const std::locale& in, const winlocale& lc, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return create_parsing_impl<char>(in, lc);
            case char_facet_t::wchar_f: return create_parsing_impl<wchar_t>(in, lc);
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // std-facet not available (yet)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return create_parsing_impl<char16_t>(in, lc);
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return create_parsing_impl<char32_t>(in, lc);
#endif
        }
        return in;
    }

}}} // namespace boost::locale::impl_win
