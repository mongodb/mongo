//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/encoding.hpp>
#include "all_generator.hpp"
#include <boost/assert.hpp>
#include <ios>
#include <locale>
#include <string>
#include <type_traits>

namespace boost { namespace locale { namespace impl_std {

    class utf8_collator_from_wide : public std::collate<char> {
    public:
        typedef std::collate<wchar_t> wfacet;
        utf8_collator_from_wide(const std::string& locale_name) :
            base_(std::locale::classic(), new std::collate_byname<wchar_t>(locale_name))
        {}
        int do_compare(const char* lb, const char* le, const char* rb, const char* re) const override
        {
            const std::wstring l = conv::utf_to_utf<wchar_t>(lb, le);
            const std::wstring r = conv::utf_to_utf<wchar_t>(rb, re);
            return std::use_facet<wfacet>(base_).compare(l.c_str(),
                                                         l.c_str() + l.size(),
                                                         r.c_str(),
                                                         r.c_str() + r.size());
        }
        long do_hash(const char* b, const char* e) const override
        {
            const std::wstring tmp = conv::utf_to_utf<wchar_t>(b, e);
            return std::use_facet<wfacet>(base_).hash(tmp.c_str(), tmp.c_str() + tmp.size());
        }
        std::string do_transform(const char* b, const char* e) const override
        {
            const std::wstring tmp = conv::utf_to_utf<wchar_t>(b, e);
            const std::wstring wkey = std::use_facet<wfacet>(base_).transform(tmp.c_str(), tmp.c_str() + tmp.size());
            // wkey is only for lexicographical sorting, so may no be valid UTF
            // --> Convert to char array in big endian order so sorting stays the same
            std::string key;
            key.reserve(wkey.size() * sizeof(wchar_t));
            for(const wchar_t c : wkey) {
                const auto tv = static_cast<std::make_unsigned<wchar_t>::type>(c);
                for(unsigned i = 1; i <= sizeof(tv); ++i)
                    key += char((tv >> (sizeof(tv) - i) * 8) & 0xFF);
            }
            return key;
        }

    private:
        std::locale base_;
    };

    // Workaround for a bug in the C++ or C standard library so far observed on the Appveyor VS2017 image
    bool collation_works(const std::locale& l)
    {
        const auto& col = std::use_facet<std::collate<char>>(l);
        const std::string a = "a";
        const std::string b = "b";
        try {
            // On some broken system libs transform throws an exception
            const auto ta = col.transform(a.c_str(), a.c_str() + a.size());
            const auto tb = col.transform(b.c_str(), b.c_str() + b.size());
            // This should always be true but on some broken system libs `l(a,b) == !l(b,a) == false`
            return l(a, b) == !l(b, a) && (l(a, b) == (ta < tb));
        } catch(const std::exception&) { // LCOV_EXCL_LINE
            return false;                // LCOV_EXCL_LINE
        }
    }

    std::locale
    create_collate(const std::locale& in, const std::string& locale_name, char_facet_t type, utf8_support utf)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f:
                if(utf == utf8_support::from_wide)
                    return std::locale(in, new utf8_collator_from_wide(locale_name));
                else {
                    std::locale res = std::locale(in, new std::collate_byname<char>(locale_name));
                    if(utf != utf8_support::none && !collation_works(res)) {
                        res = std::locale(res, new utf8_collator_from_wide(locale_name)); // LCOV_EXCL_LINE
                    }
                    BOOST_ASSERT_MSG(collation_works(res), "Broken collation");
                    return res;
                }

            case char_facet_t::wchar_f: return std::locale(in, new std::collate_byname<wchar_t>(locale_name));

#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // std-facet not available (yet)
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return std::locale(in, new std::collate_byname<char16_t>(locale_name));
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return std::locale(in, new std::collate_byname<char32_t>(locale_name));
#endif
        }
        return in;
    }

}}} // namespace boost::locale::impl_std
