//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/generator.hpp>
#if defined(__FreeBSD__)
#    include <xlocale.h>
#endif
#include "../shared/mo_hash.hpp"
#include "all_generator.hpp"
#include <clocale>
#include <cstring>
#include <ios>
#include <locale>
#include <stdexcept>
#include <string>
#include <vector>
#include <wchar.h>

namespace boost { namespace locale { namespace impl_posix {

    template<typename CharType>
    struct coll_traits;

    template<>
    struct coll_traits<char> {
        static size_t xfrm(char* out, const char* in, size_t n, locale_t l) { return strxfrm_l(out, in, n, l); }
        static size_t coll(const char* left, const char* right, locale_t l) { return strcoll_l(left, right, l); }
    };

    template<>
    struct coll_traits<wchar_t> {
        static size_t xfrm(wchar_t* out, const wchar_t* in, size_t n, locale_t l) { return wcsxfrm_l(out, in, n, l); }
        static size_t coll(const wchar_t* left, const wchar_t* right, locale_t l) { return wcscoll_l(left, right, l); }
    };

    template<typename CharType>
    class collator : public std::collate<CharType> {
    public:
        typedef std::basic_string<CharType> string_type;
        collator(std::shared_ptr<locale_t> l, size_t refs = 0) : std::collate<CharType>(refs), lc_(std::move(l)) {}

        int do_compare(const CharType* lb, const CharType* le, const CharType* rb, const CharType* re) const override
        {
            string_type left(lb, le - lb);
            string_type right(rb, re - rb);
            int res = coll_traits<CharType>::coll(left.c_str(), right.c_str(), *lc_);
            if(res < 0)
                return -1;
            if(res > 0)
                return 1;
            return 0;
        }
        long do_hash(const CharType* b, const CharType* e) const override
        {
            string_type s(do_transform(b, e));
            const char* begin = reinterpret_cast<const char*>(s.c_str());
            const char* end = begin + s.size() * sizeof(CharType);
            return gnu_gettext::pj_winberger_hash_function(begin, end);
        }
        string_type do_transform(const CharType* b, const CharType* e) const override
        {
            string_type s(b, e - b);
            std::vector<CharType> buf((e - b) * 2 + 1);
            size_t n = coll_traits<CharType>::xfrm(buf.data(), s.c_str(), buf.size(), *lc_);
            if(n > buf.size()) {
                buf.resize(n);
                coll_traits<CharType>::xfrm(buf.data(), s.c_str(), n, *lc_);
            }
            return string_type(buf.data(), n);
        }

    private:
        std::shared_ptr<locale_t> lc_;
    };

    std::locale create_collate(const std::locale& in, std::shared_ptr<locale_t> lc, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return std::locale(in, new collator<char>(std::move(lc)));
            case char_facet_t::wchar_f: return std::locale(in, new collator<wchar_t>(std::move(lc)));
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // std-facet not available (yet)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return std::locale(in, new collator<char16_t>(std::move(lc)));
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return std::locale(in, new collator<char32_t>(std::move(lc)));
#endif
        }
        return in;
    }

}}} // namespace boost::locale::impl_posix
