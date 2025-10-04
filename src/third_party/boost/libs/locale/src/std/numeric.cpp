//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/encoding.hpp>
#include <boost/locale/formatting.hpp>
#include <boost/locale/generator.hpp>
#include <algorithm>
#include <cstdlib>
#include <ios>
#include <locale>
#include <sstream>
#include <string>

#include "../util/numeric.hpp"
#include "all_generator.hpp"

namespace boost { namespace locale { namespace impl_std {
    /// Forwarding time_put facet
    /// Almost the same as `std::time_put_byname` but replaces the locale of the `ios_base` in `do_put` so that e.g.
    /// weekday names are translated and formatting is as per the language of the base locale
    template<typename CharType>
    class time_put_from_base : public std::time_put<CharType> {
    public:
        using iter_type = typename std::time_put<CharType>::iter_type;

        time_put_from_base(const std::locale& base) :
            base_facet_(std::use_facet<std::time_put<CharType>>(base)), base_ios_(nullptr)
        {
            // Imbue the ios with the base locale so the facets `do_put` uses its formatting information
            base_ios_.imbue(base);
        }

        iter_type do_put(iter_type out,
                         std::ios_base& /*ios*/,
                         CharType fill,
                         const std::tm* tm,
                         char format,
                         char modifier) const override
        {
            return base_facet_.put(out, base_ios_, fill, tm, format, modifier);
        }

    private:
        const std::time_put<CharType>& base_facet_;
        mutable std::basic_ios<CharType> base_ios_;
    };

    class utf8_time_put_from_wide : public std::time_put<char> {
    public:
        utf8_time_put_from_wide(const std::locale& base, size_t refs = 0) : std::time_put<char>(refs), base_(base) {}
        iter_type do_put(iter_type out,
                         std::ios_base& /*ios*/,
                         char fill,
                         const std::tm* tm,
                         char format,
                         char modifier = 0) const override
        {
            std::basic_ostringstream<wchar_t> wtmps;
            wtmps.imbue(base_);
            std::use_facet<std::time_put<wchar_t>>(base_)
              .put(wtmps, wtmps, wchar_t(fill), tm, wchar_t(format), wchar_t(modifier));
            const std::string tmp = conv::utf_to_utf<char>(wtmps.str());
            return std::copy(tmp.begin(), tmp.end(), out);
        }

    private:
        std::locale base_;
    };

    class utf8_numpunct_from_wide : public std::numpunct<char> {
    public:
        utf8_numpunct_from_wide(const std::locale& base, size_t refs = 0) : std::numpunct<char>(refs)
        {
            typedef std::numpunct<wchar_t> wfacet_type;
            const wfacet_type& wfacet = std::use_facet<wfacet_type>(base);

            truename_ = conv::utf_to_utf<char>(wfacet.truename());
            falsename_ = conv::utf_to_utf<char>(wfacet.falsename());

            wchar_t tmp_decimal_point = wfacet.decimal_point();
            wchar_t tmp_thousands_sep = wfacet.thousands_sep();
            std::string tmp_grouping = wfacet.grouping();

            if(32 <= tmp_thousands_sep && tmp_thousands_sep <= 126 && 32 <= tmp_decimal_point
               && tmp_decimal_point <= 126) {
                thousands_sep_ = static_cast<char>(tmp_thousands_sep);
                decimal_point_ = static_cast<char>(tmp_decimal_point);
                grouping_ = tmp_grouping;
            } else if(32 <= tmp_decimal_point && tmp_decimal_point <= 126 && tmp_thousands_sep == 0xA0) {
                // workaround common bug - substitute NBSP with ordinary space
                thousands_sep_ = ' ';
                decimal_point_ = static_cast<char>(tmp_decimal_point);
                grouping_ = tmp_grouping;
            } else if(32 <= tmp_decimal_point && tmp_decimal_point <= 126) {
                thousands_sep_ = ',';
                decimal_point_ = static_cast<char>(tmp_decimal_point);
                grouping_ = std::string();
            } else {
                thousands_sep_ = ',';
                decimal_point_ = '.';
                grouping_ = std::string();
            }
        }

        char do_decimal_point() const override { return decimal_point_; }
        char do_thousands_sep() const override { return thousands_sep_; }
        std::string do_grouping() const override { return grouping_; }
        std::string do_truename() const override { return truename_; }
        std::string do_falsename() const override { return falsename_; }

    private:
        std::string truename_;
        std::string falsename_;
        char thousands_sep_;
        char decimal_point_;
        std::string grouping_;
    };

    template<bool Intl>
    class utf8_moneypunct_from_wide : public std::moneypunct<char, Intl> {
    public:
        utf8_moneypunct_from_wide(const std::locale& base, size_t refs = 0) : std::moneypunct<char, Intl>(refs)
        {
            typedef std::moneypunct<wchar_t, Intl> wfacet_type;
            const wfacet_type& wfacet = std::use_facet<wfacet_type>(base);

            curr_symbol_ = conv::utf_to_utf<char>(wfacet.curr_symbol());
            positive_sign_ = conv::utf_to_utf<char>(wfacet.positive_sign());
            negative_sign_ = conv::utf_to_utf<char>(wfacet.negative_sign());
            frac_digits_ = wfacet.frac_digits();
            pos_format_ = wfacet.pos_format();
            neg_format_ = wfacet.neg_format();

            wchar_t tmp_decimal_point = wfacet.decimal_point();
            wchar_t tmp_thousands_sep = wfacet.thousands_sep();
            std::string tmp_grouping = wfacet.grouping();
            if(32 <= tmp_thousands_sep && tmp_thousands_sep <= 126 && 32 <= tmp_decimal_point
               && tmp_decimal_point <= 126) {
                thousands_sep_ = static_cast<char>(tmp_thousands_sep);
                decimal_point_ = static_cast<char>(tmp_decimal_point);
                grouping_ = tmp_grouping;
            } else if(32 <= tmp_decimal_point && tmp_decimal_point <= 126 && tmp_thousands_sep == 0xA0) {
                // workaround common bug - substitute NBSP with ordinary space
                thousands_sep_ = ' ';
                decimal_point_ = static_cast<char>(tmp_decimal_point);
                grouping_ = tmp_grouping;
            } else if(32 <= tmp_decimal_point && tmp_decimal_point <= 126) {
                thousands_sep_ = ',';
                decimal_point_ = static_cast<char>(tmp_decimal_point);
                grouping_ = std::string();
            } else {
                thousands_sep_ = ',';
                decimal_point_ = '.';
                grouping_ = std::string();
            }
        }

        char do_decimal_point() const override { return decimal_point_; }

        char do_thousands_sep() const override { return thousands_sep_; }

        std::string do_grouping() const override { return grouping_; }

        std::string do_curr_symbol() const override { return curr_symbol_; }
        std::string do_positive_sign() const override { return positive_sign_; }
        std::string do_negative_sign() const override { return negative_sign_; }

        int do_frac_digits() const override { return frac_digits_; }

        std::money_base::pattern do_pos_format() const override { return pos_format_; }

        std::money_base::pattern do_neg_format() const override { return neg_format_; }

    private:
        char thousands_sep_;
        char decimal_point_;
        std::string grouping_;
        std::string curr_symbol_;
        std::string positive_sign_;
        std::string negative_sign_;
        int frac_digits_;
        std::money_base::pattern pos_format_, neg_format_;
    };

    class utf8_numpunct : public std::numpunct_byname<char> {
    public:
        typedef std::numpunct_byname<char> base_type;
        utf8_numpunct(const std::string& name, size_t refs = 0) : std::numpunct_byname<char>(name, refs) {}
        char do_thousands_sep() const override
        {
            unsigned char bs = base_type::do_thousands_sep();
            if(bs > 127)
                if(bs == 0xA0)
                    return ' ';
                else
                    return 0;
            else
                return bs;
        }
        std::string do_grouping() const override
        {
            unsigned char bs = base_type::do_thousands_sep();
            if(bs > 127 && bs != 0xA0)
                return std::string();
            return base_type::do_grouping();
        }
    };

    template<bool Intl>
    class utf8_moneypunct : public std::moneypunct_byname<char, Intl> {
    public:
        typedef std::moneypunct_byname<char, Intl> base_type;
        utf8_moneypunct(const std::string& name, size_t refs = 0) : std::moneypunct_byname<char, Intl>(name, refs) {}
        char do_thousands_sep() const override
        {
            unsigned char bs = base_type::do_thousands_sep();
            if(bs > 127)
                if(bs == 0xA0)
                    return ' ';
                else
                    return 0;
            else
                return bs;
        }
        std::string do_grouping() const override
        {
            unsigned char bs = base_type::do_thousands_sep();
            if(bs > 127 && bs != 0xA0)
                return std::string();
            return base_type::do_grouping();
        }
    };

    template<typename CharType>
    std::locale create_basic_parsing(const std::locale& in, const std::string& locale_name)
    {
        std::locale tmp = std::locale(in, new std::numpunct_byname<CharType>(locale_name));
        tmp = std::locale(tmp, new std::moneypunct_byname<CharType, true>(locale_name));
        tmp = std::locale(tmp, new std::moneypunct_byname<CharType, false>(locale_name));
        tmp = std::locale(tmp, new std::ctype_byname<CharType>(locale_name));
        return std::locale(tmp, new util::base_num_parse<CharType>());
    }

    template<typename CharType>
    std::locale create_basic_formatting(const std::locale& in, const std::string& locale_name)
    {
        std::locale tmp = create_basic_parsing<CharType>(in, locale_name);
        tmp = std::locale(tmp, new time_put_from_base<CharType>(std::locale(locale_name)));
        return std::locale(tmp, new util::base_num_format<CharType>());
    }

    std::locale
    create_formatting(const std::locale& in, const std::string& locale_name, char_facet_t type, utf8_support utf)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f:
                if(utf != utf8_support::none) {
                    const std::locale base(locale_name);
                    std::time_put<char>* time_put;
                    if(utf == utf8_support::from_wide)
                        time_put = new utf8_time_put_from_wide(base);
                    else
                        time_put = new time_put_from_base<char>(base);
                    std::locale tmp(in, time_put);
                    // Fix possibly invalid UTF-8
                    tmp = std::locale(tmp, new utf8_numpunct_from_wide(base));
                    tmp = std::locale(tmp, new utf8_moneypunct_from_wide<true>(base));
                    tmp = std::locale(tmp, new utf8_moneypunct_from_wide<false>(base));
                    return std::locale(tmp, new util::base_num_format<char>());
                } else
                    return create_basic_formatting<char>(in, locale_name);
            case char_facet_t::wchar_f: return create_basic_formatting<wchar_t>(in, locale_name);
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // std-facet not available (yet)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return create_basic_formatting<char16_t>(in, locale_name);
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return create_basic_formatting<char32_t>(in, locale_name);
#endif
        }
        return in;
    }

    std::locale
    create_parsing(const std::locale& in, const std::string& locale_name, char_facet_t type, utf8_support utf)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f:
                if(utf != utf8_support::none) {
                    const std::locale base(locale_name);
                    // Fix possibly invalid UTF-8
                    std::locale tmp = std::locale(in, new utf8_numpunct_from_wide(base));
                    tmp = std::locale(tmp, new utf8_moneypunct_from_wide<true>(base));
                    tmp = std::locale(tmp, new utf8_moneypunct_from_wide<false>(base));
                    return std::locale(tmp, new util::base_num_parse<char>());
                } else
                    return create_basic_parsing<char>(in, locale_name);
            case char_facet_t::wchar_f: return create_basic_parsing<wchar_t>(in, locale_name);
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // std-facet not available (yet)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return create_basic_parsing<char16_t>(in, locale_name);
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return create_basic_parsing<char32_t>(in, locale_name);
#endif
        }
        return in;
    }

}}} // namespace boost::locale::impl_std
