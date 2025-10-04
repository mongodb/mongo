//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/encoding.hpp>
#include <boost/locale/generator.hpp>
#include <boost/locale/utf8_codecvt.hpp>
#include <boost/locale/util.hpp>
#include <boost/locale/util/string.hpp>
#include <boost/assert.hpp>
#include <algorithm>
#include <cstddef>
#include <cstring>

#include "encoding.hpp"
#include "make_std_unique.hpp"

#ifdef BOOST_MSVC
#    pragma warning(disable : 4244) // loose data
#endif

namespace boost { namespace locale { namespace util {

    base_converter::~base_converter() = default;

    class utf8_converter : public base_converter {
    public:
        int max_len() const override { return 4; }

        utf8_converter* clone() const override { return new utf8_converter(); }

        bool is_thread_safe() const override { return true; }

        utf::code_point to_unicode(const char*& begin, const char* end) override
        {
            const char* p = begin;

            const utf::code_point c = utf::utf_traits<char>::decode(p, end);

            if(c == utf::illegal)
                return illegal;

            if(c == utf::incomplete)
                return incomplete;

            begin = p;
            return c;
        }

        utf::len_or_error from_unicode(utf::code_point u, char* begin, const char* end) override
        {
            if(!utf::is_valid_codepoint(u))
                return illegal;
            int width = utf::utf_traits<char>::width(u);
            std::ptrdiff_t d = end - begin;
            if(d < width)
                return incomplete;
            utf::utf_traits<char>::encode(u, begin);
            return width;
        }
    }; // utf8_converter

    class simple_converter_impl {
    public:
        static constexpr int hash_table_size = 1024;

        simple_converter_impl(const std::string& encoding)
        {
            for(unsigned i = 0; i < 128; i++)
                to_unicode_tbl_[i] = i;
            const conv::utf_encoder<wchar_t> to_utf(encoding, conv::skip);
            for(unsigned i = 128; i < 256; i++) {
                char buf[2] = {util::to_char(i), 0};
                uint32_t uchar = utf::illegal;
                try {
                    std::wstring const tmp = to_utf.convert(buf, buf + 1);
                    if(tmp.size() == 1)
                        uchar = tmp[0];
                    else
                        uchar = utf::illegal;
                } catch(const conv::conversion_error&) { // LCOV_EXCL_LINE
                    uchar = utf::illegal;                // LCOV_EXCL_LINE
                }
                to_unicode_tbl_[i] = uchar;
            }
            for(int i = 0; i < hash_table_size; i++)
                from_unicode_tbl_[i] = 0;
            for(unsigned i = 1; i < 256; i++) {
                if(to_unicode_tbl_[i] != utf::illegal) {
                    unsigned pos = to_unicode_tbl_[i] % hash_table_size;
                    while(from_unicode_tbl_[pos] != 0)
                        pos = (pos + 1) % hash_table_size;
                    from_unicode_tbl_[pos] = i;
                }
            }
        }

        utf::code_point to_unicode(const char*& begin, const char* end) const
        {
            if(begin == end)
                return utf::incomplete;
            unsigned char c = *begin++;
            return to_unicode_tbl_[c];
        }
        utf::len_or_error from_unicode(utf::code_point u, char* begin, const char* end) const
        {
            if(begin == end)
                return utf::incomplete;
            if(u == 0) {
                *begin = 0;
                return 1;
            }
            unsigned pos = u % hash_table_size;
            unsigned char c;
            while((c = from_unicode_tbl_[pos]) != 0 && to_unicode_tbl_[c] != u)
                pos = (pos + 1) % hash_table_size;
            if(c == 0)
                return utf::illegal;
            *begin = to_char(c);
            return 1;
        }

    private:
        utf::code_point to_unicode_tbl_[256];
        unsigned char from_unicode_tbl_[hash_table_size];
    };

    class simple_converter : public base_converter {
    public:
        simple_converter(const std::string& encoding) : cvt_(encoding) {}

        int max_len() const override { return 1; }

        bool is_thread_safe() const override { return true; }
        base_converter* clone() const override { return new simple_converter(*this); }

        utf::code_point to_unicode(const char*& begin, const char* end) override { return cvt_.to_unicode(begin, end); }
        utf::len_or_error from_unicode(utf::code_point u, char* begin, const char* end) override
        {
            return cvt_.from_unicode(u, begin, end);
        }

    private:
        simple_converter_impl cvt_;
    };

    template<typename CharType>
    class simple_codecvt : public generic_codecvt<CharType, simple_codecvt<CharType>> {
    public:
        simple_codecvt(const std::string& encoding, size_t refs = 0) :
            generic_codecvt<CharType, simple_codecvt<CharType>>(refs), cvt_(encoding)
        {}

        struct state_type {};
        static state_type initial_state(generic_codecvt_base::initial_convertion_state /* unused */)
        {
            return state_type();
        }
        static int max_encoding_length() { return 1; }

        utf::code_point to_unicode(state_type&, const char*& begin, const char* end) const
        {
            return cvt_.to_unicode(begin, end);
        }

        utf::len_or_error from_unicode(state_type&, utf::code_point u, char* begin, const char* end) const
        {
            return cvt_.from_unicode(u, begin, end);
        }

    private:
        simple_converter_impl cvt_;
    };

    namespace {
        const char* simple_encoding_table[] = {
          "cp1250",      "cp1251",      "cp1252",      "cp1253",      "cp1254",      "cp1255",
          "cp1256",      "cp1257",      "iso88591",    "iso885913",   "iso885915",   "iso88592",
          "iso88593",    "iso88594",    "iso88595",    "iso88596",    "iso88597",    "iso88598",
          "iso88599",    "koi8r",       "koi8u",       "usascii",     "windows1250", "windows1251",
          "windows1252", "windows1253", "windows1254", "windows1255", "windows1256", "windows1257"};

        bool compare_strings(const char* l, const char* r)
        {
            return strcmp(l, r) < 0;
        }
    } // namespace

    std::vector<std::string> get_simple_encodings()
    {
        return std::vector<std::string>(simple_encoding_table, std::end(simple_encoding_table));
    }

    bool is_simple_encoding(const std::string& encoding)
    {
        std::string norm = util::normalize_encoding(encoding);
        return std::binary_search(simple_encoding_table,
                                  std::end(simple_encoding_table),
                                  norm.c_str(),
                                  compare_strings);
    }

    std::unique_ptr<base_converter> create_simple_converter(const std::string& encoding)
    {
        if(is_simple_encoding(encoding))
            return make_std_unique<simple_converter>(encoding);
        return nullptr;
    }

    std::unique_ptr<base_converter> create_utf8_converter()
    {
        return make_std_unique<utf8_converter>();
    }

    // clang-format off
    base_converter* create_simple_converter_new_ptr(const std::string& encoding){ return create_simple_converter(encoding).release(); } // LCOV_EXCL_LINE
    base_converter* create_utf8_converter_new_ptr(){ return create_utf8_converter().release(); } // LCOV_EXCL_LINE
    // clang-format on

    template<typename CharType, bool ThreadSafe>
    class code_converter : public generic_codecvt<CharType, code_converter<CharType, ThreadSafe>> {
    public:
        typedef std::unique_ptr<base_converter> base_converter_ptr;
        typedef base_converter_ptr state_type;

        code_converter(base_converter_ptr cvt, size_t refs = 0) :
            generic_codecvt<CharType, code_converter<CharType, ThreadSafe>>(refs), cvt_(std::move(cvt))
        {}

        int max_encoding_length() const { return cvt_->max_len(); }

        base_converter_ptr initial_state(generic_codecvt_base::initial_convertion_state /* unused */) const
        {
            base_converter_ptr r;
            if(!ThreadSafe)
                r.reset(cvt_->clone());
            return r;
        }

        utf::code_point to_unicode(base_converter_ptr& ptr, const char*& begin, const char* end) const
        {
            if(ThreadSafe)
                return cvt_->to_unicode(begin, end);
            else
                return ptr->to_unicode(begin, end);
        }

        utf::len_or_error from_unicode(base_converter_ptr& ptr, utf::code_point u, char* begin, const char* end) const
        {
            if(ThreadSafe)
                return cvt_->from_unicode(u, begin, end);
            else
                return ptr->from_unicode(u, begin, end);
        }

    private:
        base_converter_ptr cvt_;
    };

    template<typename CharType>
    static std::locale do_create_codecvt(const std::locale& in, std::unique_ptr<base_converter> cvt)
    {
        return cvt->is_thread_safe() ? std::locale(in, new code_converter<CharType, true>(std::move(cvt))) :
                                       std::locale(in, new code_converter<CharType, false>(std::move(cvt)));
    }

    std::locale create_codecvt(const std::locale& in, std::unique_ptr<base_converter> cvt, char_facet_t type)
    {
        if(!cvt)
            cvt.reset(new base_converter());
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return do_create_codecvt<char>(in, std::move(cvt));
            case char_facet_t::wchar_f: return do_create_codecvt<wchar_t>(in, std::move(cvt));
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // No std facet
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return do_create_codecvt<char16_t>(in, std::move(cvt));
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return do_create_codecvt<char32_t>(in, std::move(cvt));
#endif
        }
        return in;
    }

    /// Install utf8 codecvt to UTF-16 or UTF-32 into locale \a in and return
    /// new locale that is based on \a in and uses new facet.
    std::locale create_utf8_codecvt(const std::locale& in, char_facet_t type)
    {
        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return std::locale(in, new utf8_codecvt<char>());
            case char_facet_t::wchar_f: return std::locale(in, new utf8_codecvt<wchar_t>());
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // No std facet
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return std::locale(in, new utf8_codecvt<char16_t>());
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return std::locale(in, new utf8_codecvt<char32_t>());
#endif
        }
        return in;
    }

    std::locale create_simple_codecvt(const std::locale& in, const std::string& encoding, char_facet_t type)
    {
        if(!is_simple_encoding(encoding))
            throw boost::locale::conv::invalid_charset_error("Invalid simple encoding " + encoding);

        switch(type) {
            case char_facet_t::nochar: break;
            case char_facet_t::char_f: return std::locale(in, new simple_codecvt<char>(encoding));
            case char_facet_t::wchar_f: return std::locale(in, new simple_codecvt<wchar_t>(encoding));
#ifdef __cpp_char8_t
            case char_facet_t::char8_f: break; // No std facet
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            case char_facet_t::char16_f: return std::locale(in, new simple_codecvt<char16_t>(encoding));
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            case char_facet_t::char32_f: return std::locale(in, new simple_codecvt<char32_t>(encoding));
#endif
        }
        return in;
    }

}}} // namespace boost::locale::util
