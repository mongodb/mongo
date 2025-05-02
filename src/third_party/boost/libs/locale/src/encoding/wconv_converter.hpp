//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_WCONV_CODEPAGE_HPP
#define BOOST_LOCALE_IMPL_WCONV_CODEPAGE_HPP

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <boost/locale/encoding.hpp>
#include "../util/encoding.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <windows.h>

namespace boost { namespace locale { namespace conv { namespace impl {

    void multibyte_to_wide_one_by_one(int codepage, const char* begin, const char* end, std::vector<wchar_t>& buf)
    {
        buf.reserve(end - begin);
        DWORD flags = MB_ERR_INVALID_CHARS;
        while(begin != end) {
            wchar_t wide_buf[4];
            int n = 0;
            int len = IsDBCSLeadByteEx(codepage, *begin) ? 2 : 1;
            if(len == 2 && begin + 1 == end)
                return;
            n = MultiByteToWideChar(codepage, flags, begin, len, wide_buf, 4);
            if(n == 0 && flags != 0 && GetLastError() == ERROR_INVALID_FLAGS) {
                flags = 0;
                n = MultiByteToWideChar(codepage, flags, begin, len, wide_buf, 4);
            }
            for(int i = 0; i < n; i++)
                buf.push_back(wide_buf[i]);
            begin += len;
        }
    }

    void multibyte_to_wide(int codepage, const char* begin, const char* end, bool do_skip, std::vector<wchar_t>& buf)
    {
        if(begin == end)
            return;
        const std::ptrdiff_t num_chars = end - begin;
        if(num_chars > std::numeric_limits<int>::max())
            throw conversion_error();
        DWORD flags = MB_ERR_INVALID_CHARS;
        int n = MultiByteToWideChar(codepage, flags, begin, static_cast<int>(num_chars), 0, 0);
        if(n == 0 && GetLastError() == ERROR_INVALID_FLAGS) {
            flags = 0;
            n = MultiByteToWideChar(codepage, flags, begin, static_cast<int>(num_chars), 0, 0);
        }

        if(n == 0) {
            if(do_skip) {
                multibyte_to_wide_one_by_one(codepage, begin, end, buf);
                return;
            }
            throw conversion_error();
        }

        buf.resize(n);
        if(MultiByteToWideChar(codepage, flags, begin, static_cast<int>(num_chars), buf.data(), n) == 0)
            throw conversion_error();
    }

    void wide_to_multibyte_non_zero(int codepage,
                                    const wchar_t* begin,
                                    const wchar_t* end,
                                    bool do_skip,
                                    std::vector<char>& buf)
    {
        buf.clear();
        if(begin == end)
            return;
        BOOL substitute = FALSE;
        BOOL* substitute_ptr = (codepage == CP_UTF7 || codepage == CP_UTF8) ? nullptr : &substitute;
        char subst_char = 0;
        char* subst_char_ptr = (codepage == CP_UTF7 || codepage == CP_UTF8) ? nullptr : &subst_char;

        if((end - begin) > std::numeric_limits<int>::max())
            throw conversion_error();
        const int num_chars = static_cast<int>(end - begin);
        int n = WideCharToMultiByte(codepage, 0, begin, num_chars, nullptr, 0, subst_char_ptr, substitute_ptr);
        // Some codepages don't support substitutions
        if(n == 0 && GetLastError() == ERROR_INVALID_PARAMETER) {
            subst_char_ptr = nullptr;
            substitute_ptr = nullptr;
            n = WideCharToMultiByte(codepage, 0, begin, num_chars, nullptr, 0, subst_char_ptr, substitute_ptr);
        }
        buf.resize(n);

        if(WideCharToMultiByte(codepage, 0, begin, num_chars, buf.data(), n, subst_char_ptr, substitute_ptr) == 0)
            throw conversion_error();
        if(substitute) {
            if(do_skip)
                buf.erase(std::remove(buf.begin(), buf.end(), subst_char), buf.end());
            else
                throw conversion_error();
        }
    }

    void wide_to_multibyte(int codepage, const wchar_t* begin, const wchar_t* end, bool do_skip, std::vector<char>& buf)
    {
        if(begin == end)
            return;
        buf.reserve(end - begin);
        const wchar_t* e = std::find(begin, end, L'\0');
        const wchar_t* b = begin;
        std::vector<char> tmp;
        for(;;) {
            wide_to_multibyte_non_zero(codepage, b, e, do_skip, tmp);
            const size_t osize = buf.size();
            buf.resize(osize + tmp.size());
            std::copy(tmp.begin(), tmp.end(), buf.begin() + osize);
            if(e == end)
                break;
            buf.push_back('\0');
            b = e + 1;
            e = std::find(b, end, L'0');
        }
    }

    template<typename CharType>
    bool validate_utf16(const CharType* str, size_t len)
    {
        const CharType* begin = str;
        const CharType* end = str + len;
        while(begin != end) {
            utf::code_point c = utf::utf_traits<CharType, 2>::decode(begin, end);
            if(c == utf::illegal || c == utf::incomplete)
                return false;
        }
        return true;
    }

    template<typename CharType, typename OutChar>
    void clean_invalid_utf16(const CharType* str, size_t len, std::vector<OutChar>& out)
    {
        out.reserve(len);
        for(size_t i = 0; i < len; i++) {
            uint16_t c = static_cast<uint16_t>(str[i]);

            if(0xD800 <= c && c <= 0xDBFF) {
                i++;
                if(i >= len)
                    return;
                uint16_t c2 = static_cast<uint16_t>(str[i]);
                if(0xDC00 <= c2 && c2 <= 0xDFFF) {
                    out.push_back(static_cast<OutChar>(c));
                    out.push_back(static_cast<OutChar>(c2));
                }
            } else if(0xDC00 <= c && c <= 0xDFFF)
                continue;
            else
                out.push_back(static_cast<OutChar>(c));
        }
    }

    class wconv_between final : public detail::narrow_converter {
    public:
        wconv_between() : how_(skip), to_code_page_(-1), from_code_page_(-1) {}
        bool open(const std::string& to_charset, const std::string& from_charset, method_type how)
        {
            how_ = how;
            to_code_page_ = util::encoding_to_windows_codepage(to_charset);
            from_code_page_ = util::encoding_to_windows_codepage(from_charset);
            if(to_code_page_ == -1 || from_code_page_ == -1)
                return false;
            return true;
        }

        template<typename Char>
        std::basic_string<Char> convert(const char* begin, const char* end)
        {
            static_assert(sizeof(Char) == sizeof(char), "Not a narrow char type");
            if(to_code_page_ == CP_UTF8 && from_code_page_ == CP_UTF8)
                return utf_to_utf<Char>(begin, end, how_);

            std::basic_string<Char> res;

            std::vector<wchar_t> tmp; // buffer for mb2w
            std::wstring tmps;        // buffer for utf_to_utf
            const wchar_t* wbegin = nullptr;
            const wchar_t* wend = nullptr;

            if(from_code_page_ == CP_UTF8) {
                tmps = utf_to_utf<wchar_t>(begin, end, how_);
                if(tmps.empty())
                    return res;
                wbegin = tmps.c_str();
                wend = wbegin + tmps.size();
            } else {
                multibyte_to_wide(from_code_page_, begin, end, how_ == skip, tmp);
                if(tmp.empty())
                    return res;
                wbegin = tmp.data();
                wend = wbegin + tmp.size();
            }

            if(to_code_page_ == CP_UTF8)
                return utf_to_utf<Char>(wbegin, wend, how_);

            std::vector<char> ctmp;
            wide_to_multibyte(to_code_page_, wbegin, wend, how_ == skip, ctmp);
            if(ctmp.empty())
                return res;
            res.assign(reinterpret_cast<const Char*>(ctmp.data()), ctmp.size());
            return res;
        }

        std::string convert(const char* begin, const char* end) override { return convert<char>(begin, end); }

    private:
        method_type how_;
        int to_code_page_;
        int from_code_page_;
    };

    template<typename CharType, int size = sizeof(CharType)>
    class wconv_to_utf;

    template<typename CharType, int size = sizeof(CharType)>
    class wconv_from_utf;

    template<typename CharType>
    class wconv_to_utf<CharType, 1> final : public detail::utf_encoder<CharType> {
    public:
        bool open(const std::string& cs, method_type how) { return cvt.open("UTF-8", cs, how); }
        std::basic_string<CharType> convert(const char* begin, const char* end) override
        {
            return cvt.convert<CharType>(begin, end);
        }

    private:
        wconv_between cvt;
    };

    template<typename CharType>
    class wconv_from_utf<CharType, 1> final : public detail::utf_decoder<CharType> {
    public:
        bool open(const std::string& cs, method_type how) { return cvt.open(cs, "UTF-8", how); }
        std::string convert(const CharType* begin, const CharType* end) override
        {
            return cvt.convert(reinterpret_cast<const char*>(begin), reinterpret_cast<const char*>(end));
        }

    private:
        wconv_between cvt;
    };

    template<typename CharType>
    class wconv_to_utf<CharType, 2> final : public detail::utf_encoder<CharType> {
    public:
        using string_type = std::basic_string<CharType>;

        wconv_to_utf() : how_(skip), code_page_(-1) {}

        bool open(const std::string& charset, method_type how)
        {
            how_ = how;
            code_page_ = util::encoding_to_windows_codepage(charset);
            return code_page_ != -1;
        }

        string_type convert(const char* begin, const char* end) override
        {
            if(code_page_ == CP_UTF8)
                return utf_to_utf<CharType>(begin, end, how_);
            std::vector<wchar_t> tmp;
            multibyte_to_wide(code_page_, begin, end, how_ == skip, tmp);
            string_type res;
            if(!tmp.empty()) {
                static_assert(sizeof(CharType) == sizeof(wchar_t), "Cast not possible");
                res.assign(reinterpret_cast<CharType*>(tmp.data()), tmp.size());
            }
            return res;
        }

    private:
        method_type how_;
        int code_page_;
    };

    template<typename CharType>
    class wconv_from_utf<CharType, 2> final : public detail::utf_decoder<CharType> {
    public:
        wconv_from_utf() : how_(skip), code_page_(-1) {}

        bool open(const std::string& charset, method_type how)
        {
            how_ = how;
            code_page_ = util::encoding_to_windows_codepage(charset);
            return code_page_ != -1;
        }

        std::string convert(const CharType* begin, const CharType* end) override
        {
            if(code_page_ == CP_UTF8)
                return utf_to_utf<char>(begin, end, how_);
            const wchar_t* wbegin;
            const wchar_t* wend;
            std::vector<wchar_t> buffer; // if needed
            if(validate_utf16(begin, end - begin)) {
                static_assert(sizeof(CharType) == sizeof(wchar_t), "Cast not possible");
                wbegin = reinterpret_cast<const wchar_t*>(begin);
                wend = reinterpret_cast<const wchar_t*>(end);
            } else {
                if(how_ == stop)
                    throw conversion_error();
                else {
                    clean_invalid_utf16(begin, end - begin, buffer);
                    if(buffer.empty())
                        wbegin = wend = nullptr;
                    else {
                        wbegin = buffer.data();
                        wend = wbegin + buffer.size();
                    }
                }
            }
            std::string res;
            if(wbegin == wend)
                return res;
            std::vector<char> ctmp;
            wide_to_multibyte(code_page_, wbegin, wend, how_ == skip, ctmp);
            if(ctmp.empty())
                return res;
            res.assign(ctmp.data(), ctmp.size());
            return res;
        }

    private:
        method_type how_;
        int code_page_;
    };

    template<typename CharType>
    class wconv_to_utf<CharType, 4> final : public detail::utf_encoder<CharType> {
    public:
        using string_type = std::basic_string<CharType>;

        wconv_to_utf() : how_(skip), code_page_(-1) {}

        bool open(const std::string& charset, method_type how)
        {
            how_ = how;
            code_page_ = util::encoding_to_windows_codepage(charset);
            return code_page_ != -1;
        }

        string_type convert(const char* begin, const char* end) override
        {
            if(code_page_ == CP_UTF8)
                return utf_to_utf<CharType>(begin, end, how_);
            std::vector<wchar_t> buf;
            multibyte_to_wide(code_page_, begin, end, how_ == skip, buf);

            if(buf.empty())
                return string_type();

            return utf_to_utf<CharType>(buf.data(), buf.data() + buf.size(), how_);
        }

    private:
        method_type how_;
        int code_page_;
    };

    template<typename CharType>
    class wconv_from_utf<CharType, 4> final : public detail::utf_decoder<CharType> {
    public:
        wconv_from_utf() : how_(skip), code_page_(-1) {}

        bool open(const std::string& charset, method_type how)
        {
            how_ = how;
            code_page_ = util::encoding_to_windows_codepage(charset);
            return code_page_ != -1;
        }

        std::string convert(const CharType* begin, const CharType* end) override
        {
            if(code_page_ == CP_UTF8)
                return utf_to_utf<char>(begin, end, how_);
            std::wstring tmp = utf_to_utf<wchar_t>(begin, end, how_);

            std::vector<char> ctmp;
            wide_to_multibyte(code_page_, tmp.c_str(), tmp.c_str() + tmp.size(), how_ == skip, ctmp);
            std::string res;
            if(ctmp.empty())
                return res;
            res.assign(ctmp.data(), ctmp.size());
            return res;
        }

    private:
        method_type how_;
        int code_page_;
    };

}}}} // namespace boost::locale::conv::impl

// boostinspect:nominmax
#endif
