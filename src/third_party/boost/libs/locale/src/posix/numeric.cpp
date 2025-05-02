//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#if defined(__FreeBSD__)
#    include <xlocale.h>
#endif
#include <boost/locale/encoding.hpp>
#include <boost/locale/formatting.hpp>
#include <boost/locale/generator.hpp>
#include <boost/predef/os.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <ios>
#include <langinfo.h>
#include <locale>
#include <memory>
#include <monetary.h>
#include <sstream>
#include <string>
#include <vector>
#include <wctype.h>

#include "../util/numeric.hpp"
#include "all_generator.hpp"

namespace boost { namespace locale { namespace impl_posix {

    template<typename CharType>
    class num_format : public util::base_num_format<CharType> {
    public:
        typedef typename std::num_put<CharType>::iter_type iter_type;
        typedef std::basic_string<CharType> string_type;

        num_format(std::shared_ptr<locale_t> lc, size_t refs = 0) :
            util::base_num_format<CharType>(refs), lc_(std::move(lc))
        {}

    protected:
        iter_type do_format_currency(bool intl,
                                     iter_type out,
                                     std::ios_base& /*ios*/,
                                     CharType /*fill*/,
                                     long double val) const override
        {
            char buf[4] = {};
            const char* format = intl ? "%i" : "%n";
            errno = 0;
            ssize_t n = strfmon_l(buf, sizeof(buf), *lc_, format, static_cast<double>(val));
            if(n >= 0)
                return write_it(out, buf, n);

            for(std::vector<char> tmp(sizeof(buf) * 2); tmp.size() <= 4098; tmp.resize(tmp.size() * 2)) {
                n = strfmon_l(tmp.data(), tmp.size(), *lc_, format, static_cast<double>(val));
                if(n >= 0)
                    return write_it(out, tmp.data(), n);
            }
            return out;
        }

        std::ostreambuf_iterator<char> write_it(std::ostreambuf_iterator<char> out, const char* ptr, size_t n) const
        {
            return std::copy_n(ptr, n, out);
        }

        std::ostreambuf_iterator<wchar_t>
        write_it(std::ostreambuf_iterator<wchar_t> out, const char* ptr, size_t n) const
        {
            const std::wstring tmp = conv::to_utf<wchar_t>(ptr, ptr + n, nl_langinfo_l(CODESET, *lc_));
            return std::copy(tmp.begin(), tmp.end(), out);
        }

    private:
        std::shared_ptr<locale_t> lc_;

    }; // num_format

    namespace {
        std::string do_ftime(const char* format, const struct tm* t, locale_t lc)
        {
            char buf[16];
            size_t n = strftime_l(buf, sizeof(buf), format, t, lc);
            if(n == 0) {
                // Note standard specifies that in case of error the function returns 0,
                // however 0 may be actually valid output value of for example empty format
                // or an output of %p in some locales
                //
                // Thus we try to guess that 1024 would be enough.
                std::vector<char> v(1024);
                n = strftime_l(v.data(), 1024, format, t, lc);
                return std::string(v.data(), n);
            }
            return std::string(buf, n);
        }
        template<typename CharType>
        std::basic_string<CharType> do_ftime(const CharType* format, const struct tm* t, locale_t lc)
        {
            const std::string encoding = nl_langinfo_l(CODESET, lc);
            const std::string nformat = conv::from_utf(format, encoding);
            const std::string nres = do_ftime(nformat.c_str(), t, lc);
            return conv::to_utf<CharType>(nres, encoding);
        }
    } // namespace

    template<typename CharType>
    class time_put_posix : public std::time_put<CharType> {
    public:
        time_put_posix(std::shared_ptr<locale_t> lc, size_t refs = 0) :
            std::time_put<CharType>(refs), lc_(std::move(lc))
        {}
        typedef typename std::time_put<CharType>::iter_type iter_type;
        typedef std::basic_string<CharType> string_type;

        iter_type do_put(iter_type out,
                         std::ios_base& /*ios*/,
                         CharType /*fill*/,
                         const std::tm* tm,
                         char format,
                         char modifier) const override
        {
            CharType fmt[4] = {'%',
                               static_cast<CharType>(modifier != 0 ? modifier : format),
                               static_cast<CharType>(modifier == 0 ? '\0' : format)};
            string_type res = do_ftime(fmt, tm, *lc_);
            return std::copy(res.begin(), res.end(), out);
        }

    private:
        std::shared_ptr<locale_t> lc_;
    };

    template<typename CharType>
    class ctype_posix;

    template<>
    class ctype_posix<char> : public std::ctype<char> {
    public:
        ctype_posix(std::shared_ptr<locale_t> lc) : lc_(std::move(lc)) {}

        bool do_is(mask m, char c) const
        {
            if((m & space) && isspace_l(c, *lc_))
                return true;
            if((m & print) && isprint_l(c, *lc_))
                return true;
            if((m & cntrl) && iscntrl_l(c, *lc_))
                return true;
            if((m & upper) && isupper_l(c, *lc_))
                return true;
            if((m & lower) && islower_l(c, *lc_))
                return true;
            if((m & alpha) && isalpha_l(c, *lc_))
                return true;
            if((m & digit) && isdigit_l(c, *lc_))
                return true;
            if((m & xdigit) && isxdigit_l(c, *lc_))
                return true;
            if((m & punct) && ispunct_l(c, *lc_))
                return true;
            return false;
        }
        const char* do_is(const char* begin, const char* end, mask* m) const
        {
            while(begin != end) {
                char c = *begin++;
                int r = 0;
                if(isspace_l(c, *lc_))
                    r |= space;
                if(isprint_l(c, *lc_))
                    r |= cntrl;
                if(iscntrl_l(c, *lc_))
                    r |= space;
                if(isupper_l(c, *lc_))
                    r |= upper;
                if(islower_l(c, *lc_))
                    r |= lower;
                if(isalpha_l(c, *lc_))
                    r |= alpha;
                if(isdigit_l(c, *lc_))
                    r |= digit;
                if(isxdigit_l(c, *lc_))
                    r |= xdigit;
                if(ispunct_l(c, *lc_))
                    r |= punct;
                // r actually should be mask, but some standard
                // libraries (like STLPort)
                // do not define operator | properly so using int+cast
                *m++ = static_cast<mask>(r);
            }
            return begin;
        }
        const char* do_scan_is(mask m, const char* begin, const char* end) const
        {
            while(begin != end)
                if(do_is(m, *begin))
                    return begin;
            return begin;
        }
        const char* do_scan_not(mask m, const char* begin, const char* end) const
        {
            while(begin != end)
                if(!do_is(m, *begin))
                    return begin;
            return begin;
        }
        char toupper(char c) const { return toupper_l(c, *lc_); }
        const char* toupper(char* begin, const char* end) const
        {
            for(; begin != end; begin++)
                *begin = toupper_l(*begin, *lc_);
            return begin;
        }
        char tolower(char c) const { return tolower_l(c, *lc_); }
        const char* tolower(char* begin, const char* end) const
        {
            for(; begin != end; begin++)
                *begin = tolower_l(*begin, *lc_);
            return begin;
        }

    private:
        std::shared_ptr<locale_t> lc_;
    };

    template<>
    class ctype_posix<wchar_t> : public std::ctype<wchar_t> {
    public:
        ctype_posix(std::shared_ptr<locale_t> lc) : lc_(std::move(lc)) {}

        bool do_is(mask m, wchar_t c) const
        {
            if((m & space) && iswspace_l(c, *lc_))
                return true;
            if((m & print) && iswprint_l(c, *lc_))
                return true;
            if((m & cntrl) && iswcntrl_l(c, *lc_))
                return true;
            if((m & upper) && iswupper_l(c, *lc_))
                return true;
            if((m & lower) && iswlower_l(c, *lc_))
                return true;
            if((m & alpha) && iswalpha_l(c, *lc_))
                return true;
            if((m & digit) && iswdigit_l(c, *lc_))
                return true;
            if((m & xdigit) && iswxdigit_l(c, *lc_))
                return true;
            if((m & punct) && iswpunct_l(c, *lc_))
                return true;
            return false;
        }
        const wchar_t* do_is(const wchar_t* begin, const wchar_t* end, mask* m) const
        {
            while(begin != end) {
                wchar_t c = *begin++;
                int r = 0;
                if(iswspace_l(c, *lc_))
                    r |= space;
                if(iswprint_l(c, *lc_))
                    r |= cntrl;
                if(iswcntrl_l(c, *lc_))
                    r |= space;
                if(iswupper_l(c, *lc_))
                    r |= upper;
                if(iswlower_l(c, *lc_))
                    r |= lower;
                if(iswalpha_l(c, *lc_))
                    r |= alpha;
                if(iswdigit_l(c, *lc_))
                    r |= digit;
                if(iswxdigit_l(c, *lc_))
                    r |= xdigit;
                if(iswpunct_l(c, *lc_))
                    r |= punct;
                // r actually should be mask, but some standard
                // libraries (like STLPort)
                // do not define operator | properly so using int+cast
                *m++ = static_cast<mask>(r);
            }
            return begin;
        }
        const wchar_t* do_scan_is(mask m, const wchar_t* begin, const wchar_t* end) const
        {
            while(begin != end)
                if(do_is(m, *begin))
                    return begin;
            return begin;
        }
        const wchar_t* do_scan_not(mask m, const wchar_t* begin, const wchar_t* end) const
        {
            while(begin != end)
                if(!do_is(m, *begin))
                    return begin;
            return begin;
        }
        wchar_t toupper(wchar_t c) const { return towupper_l(c, *lc_); }
        const wchar_t* toupper(wchar_t* begin, const wchar_t* end) const
        {
            for(; begin != end; begin++)
                *begin = towupper_l(*begin, *lc_);
            return begin;
        }
        wchar_t tolower(wchar_t c) const { return tolower_l(c, *lc_); }
        const wchar_t* tolower(wchar_t* begin, const wchar_t* end) const
        {
            for(; begin != end; begin++)
                *begin = tolower_l(*begin, *lc_);
            return begin;
        }

    private:
        std::shared_ptr<locale_t> lc_;
    };

    struct basic_numpunct {
        std::string grouping;
        std::string thousands_sep;
        std::string decimal_point;
        basic_numpunct() : decimal_point(".") {}
        basic_numpunct(locale_t lc)
        {
#if defined(__APPLE__) || defined(__FreeBSD__)
            lconv* cv = localeconv_l(lc);
            grouping = cv->grouping;
            thousands_sep = cv->thousands_sep;
            decimal_point = cv->decimal_point;
#else
            thousands_sep = nl_langinfo_l(THOUSEP, lc);
            decimal_point = nl_langinfo_l(RADIXCHAR, lc);
#    ifdef GROUPING
            grouping = nl_langinfo_l(GROUPING, lc);
#    endif
#endif
        }
    };

    template<typename CharType>
    class num_punct_posix : public std::numpunct<CharType> {
    public:
        typedef std::basic_string<CharType> string_type;
        num_punct_posix(locale_t lc, size_t refs = 0) : std::numpunct<CharType>(refs)
        {
            basic_numpunct np(lc);
            to_str(np.thousands_sep, thousands_sep_, lc);
            to_str(np.decimal_point, decimal_point_, lc);
            grouping_ = np.grouping;
            if(thousands_sep_.size() > 1)
                grouping_ = std::string();
            if(decimal_point_.size() > 1)
                decimal_point_ = CharType('.');
        }
        void to_str(std::string& s1, std::string& s2, locale_t /*lc*/) { s2.swap(s1); }
        void to_str(std::string& s1, std::wstring& s2, locale_t lc)
        {
            s2 = conv::to_utf<wchar_t>(s1, nl_langinfo_l(CODESET, lc));
        }
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
    std::locale create_formatting_impl(const std::locale& in, std::shared_ptr<locale_t> lc)
    {
        std::locale tmp = std::locale(in, new num_punct_posix<CharType>(*lc));
        tmp = std::locale(tmp, new ctype_posix<CharType>(lc));
        tmp = std::locale(tmp, new time_put_posix<CharType>(lc));
        tmp = std::locale(tmp, new num_format<CharType>(std::move(lc)));
        return tmp;
    }

    template<typename CharType>
    std::locale create_parsing_impl(const std::locale& in, std::shared_ptr<locale_t> lc)
    {
        std::locale tmp = std::locale(in, new num_punct_posix<CharType>(*lc));
        tmp = std::locale(tmp, new ctype_posix<CharType>(std::move(lc)));
        tmp = std::locale(tmp, new util::base_num_parse<CharType>());
        return tmp;
    }

    std::locale create_formatting(const std::locale& in, std::shared_ptr<locale_t> lc, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return create_formatting_impl<char>(in, std::move(lc));
            case char_facet_t::wchar_f: return create_formatting_impl<wchar_t>(in, std::move(lc));
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

    std::locale create_parsing(const std::locale& in, std::shared_ptr<locale_t> lc, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return create_parsing_impl<char>(in, std::move(lc));
            case char_facet_t::wchar_f: return create_parsing_impl<wchar_t>(in, std::move(lc));
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

}}} // namespace boost::locale::impl_posix
