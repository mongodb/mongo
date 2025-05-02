//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/encoding.hpp>
#include <boost/locale/generator.hpp>
#include "../shared/mo_hash.hpp"
#include "../shared/std_collate_adapter.hpp"
#include "api.hpp"
#include <ios>
#include <locale>
#include <string>
#include <type_traits>

namespace boost { namespace locale { namespace impl_win {
    template<typename CharT, typename Result>
    using enable_if_sizeof_wchar_t = typename std::enable_if<sizeof(CharT) == sizeof(wchar_t), Result>::type;
    template<typename CharT, typename Result>
    using disable_if_sizeof_wchar_t = typename std::enable_if<sizeof(CharT) != sizeof(wchar_t), Result>::type;

    namespace {
        template<typename CharT>
        enable_if_sizeof_wchar_t<CharT, int> compare_impl(collate_level level,
                                                          const CharT* lb,
                                                          const CharT* le,
                                                          const CharT* rb,
                                                          const CharT* re,
                                                          const winlocale& wl)
        {
            return wcscoll_l(level,
                             reinterpret_cast<const wchar_t*>(lb),
                             reinterpret_cast<const wchar_t*>(le),
                             reinterpret_cast<const wchar_t*>(rb),
                             reinterpret_cast<const wchar_t*>(re),
                             wl);
        }

        template<typename CharT>
        disable_if_sizeof_wchar_t<CharT, int> compare_impl(collate_level level,
                                                           const CharT* lb,
                                                           const CharT* le,
                                                           const CharT* rb,
                                                           const CharT* re,
                                                           const winlocale& wl)
        {
            const std::wstring l = conv::utf_to_utf<wchar_t>(lb, le);
            const std::wstring r = conv::utf_to_utf<wchar_t>(rb, re);
            return wcscoll_l(level, l.c_str(), l.c_str() + l.size(), r.c_str(), r.c_str() + r.size(), wl);
        }

        template<typename CharT>
        enable_if_sizeof_wchar_t<CharT, std::wstring>
        normalize_impl(collate_level level, const CharT* b, const CharT* e, const winlocale& l)
        {
            return wcsxfrm_l(level, reinterpret_cast<const wchar_t*>(b), reinterpret_cast<const wchar_t*>(e), l);
        }

        template<typename CharT>
        disable_if_sizeof_wchar_t<CharT, std::wstring>
        normalize_impl(collate_level level, const CharT* b, const CharT* e, const winlocale& l)
        {
            const std::wstring tmp = conv::utf_to_utf<wchar_t>(b, e);
            return wcsxfrm_l(level, tmp.c_str(), tmp.c_str() + tmp.size(), l);
        }

        template<typename CharT>
        typename std::enable_if<sizeof(CharT) == 1, std::basic_string<CharT>>::type
        transform_impl(collate_level level, const CharT* b, const CharT* e, const winlocale& l)
        {
            const std::wstring wkey = normalize_impl(level, b, e, l);

            std::basic_string<CharT> key;
            BOOST_LOCALE_START_CONST_CONDITION
            if(sizeof(wchar_t) == 2)
                key.reserve(wkey.size() * 2);
            else
                key.reserve(wkey.size() * 3);
            for(const wchar_t c : wkey) {
                if(sizeof(wchar_t) == 2) {
                    const uint16_t tv = static_cast<uint16_t>(c);
                    key += CharT(tv >> 8);
                    key += CharT(tv & 0xFF);
                } else { // 4
                    const uint32_t tv = static_cast<uint32_t>(c);
                    // 21 bit
                    key += CharT((tv >> 16) & 0xFF);
                    key += CharT((tv >> 8) & 0xFF);
                    key += CharT(tv & 0xFF);
                }
            }
            BOOST_LOCALE_END_CONST_CONDITION
            return key;
        }

        template<typename CharT>
        typename std::enable_if<std::is_same<CharT, wchar_t>::value, std::wstring>::type
        transform_impl(collate_level level, const CharT* b, const CharT* e, const winlocale& l)
        {
            return normalize_impl(level, b, e, l);
        }

        template<typename CharT>
        typename std::enable_if<sizeof(CharT) >= sizeof(wchar_t) && !std::is_same<CharT, wchar_t>::value,
                                std::basic_string<CharT>>::type
        transform_impl(collate_level level, const CharT* b, const CharT* e, const winlocale& l)
        {
            const std::wstring wkey = normalize_impl(level, b, e, l);
            return std::basic_string<CharT>(wkey.begin(), wkey.end());
        }
    } // namespace

    template<typename CharT>
    class utf_collator : public collator<CharT> {
    public:
        using typename collator<CharT>::string_type;

        explicit utf_collator(winlocale lc) : collator<CharT>(), lc_(lc) {}

        int do_compare(collate_level level,
                       const CharT* lb,
                       const CharT* le,
                       const CharT* rb,
                       const CharT* re) const override
        {
            return compare_impl(level, lb, le, rb, re, lc_);
        }
        long do_hash(collate_level level, const CharT* b, const CharT* e) const override
        {
            const std::wstring key = normalize_impl(level, b, e, lc_);
            return gnu_gettext::pj_winberger_hash_function(reinterpret_cast<const char*>(key.c_str()),
                                                           reinterpret_cast<const char*>(key.c_str() + key.size()));
        }
        string_type do_transform(collate_level level, const CharT* b, const CharT* e) const override
        {
            return transform_impl(level, b, e, lc_);
        }

    private:
        winlocale lc_;
    };

    std::locale create_collate(const std::locale& in, const winlocale& lc, char_facet_t type)
    {
        if(lc.is_c()) {
            switch(type) {
                case char_facet_t::nochar: break;
                case char_facet_t::char_f: return std::locale(in, new std::collate_byname<char>("C"));
                case char_facet_t::wchar_f: return std::locale(in, new std::collate_byname<wchar_t>("C"));
#ifdef __cpp_char8_t
                case char_facet_t::char8_f: break; // std-facet not available (yet)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
                case char_facet_t::char16_f: return std::locale(in, new collate_byname<char16_t>("C"));
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
                case char_facet_t::char32_f: return std::locale(in, new collate_byname<char32_t>("C"));
#endif
            }
        } else {
            switch(type) {
                case char_facet_t::nochar: break;
                case char_facet_t::char_f: return impl::create_collators<char, utf_collator<char>>(in, lc);
                case char_facet_t::wchar_f: return impl::create_collators<wchar_t, utf_collator<wchar_t>>(in, lc);
#ifdef __cpp_char8_t
                case char_facet_t::char8_f:
                    return std::locale(in, new utf_collator<char8_t>(lc)); // std-facet not available (yet)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
                case char_facet_t::char16_f: return impl::create_collators<char16_t, utf_collator<char16_t>>(in, lc);
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
                case char_facet_t::char32_f: return impl::create_collators<char32_t, utf_collator<char32_t>>(in, lc);
#endif
            }
        }
        return in;
    }

}}} // namespace boost::locale::impl_win
