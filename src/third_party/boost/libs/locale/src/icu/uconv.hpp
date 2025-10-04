//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2020-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_SRC_LOCALE_ICU_UCONV_HPP
#define BOOST_SRC_LOCALE_ICU_UCONV_HPP

#include <boost/locale/encoding.hpp>
#include "icu_util.hpp"
#include <boost/core/exchange.hpp>

#include <memory>
#include <string>
#include <unicode/ucnv.h>
#include <unicode/unistr.h>
#include <unicode/ustring.h>
#include <unicode/utf.h>
#include <unicode/utf16.h>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4244) // 'argument' : conversion from 'int'
#    pragma warning(disable : 4267) // 'argument' : conversion from 'size_t'
#endif

namespace boost { namespace locale { namespace impl_icu {

    class icu_handle {
        UConverter* h_;
        void close()
        {
            if(h_)
                ucnv_close(h_);
        }

    public:
        explicit icu_handle(UConverter* h = nullptr) : h_(h) {}

        icu_handle(const icu_handle& rhs) = delete;
        icu_handle(icu_handle&& rhs) noexcept : h_(exchange(rhs.h_, nullptr)) {}

        icu_handle& operator=(const icu_handle& rhs) = delete;
        icu_handle& operator=(icu_handle&& rhs) noexcept
        {
            h_ = exchange(rhs.h_, nullptr);
            return *this;
        }
        icu_handle& operator=(UConverter* h)
        {
            close();
            h_ = h;
            return *this;
        }
        ~icu_handle() { close(); }

        operator UConverter*() const { return h_; }
        explicit operator bool() const { return h_ != nullptr; }
    };

    enum class cpcvt_type { skip, stop };

    struct uconv {
        uconv(const uconv& other) = delete;
        void operator=(const uconv& other) = delete;

        uconv(const std::string& charset, cpcvt_type cvt_type = cpcvt_type::skip)
        {
            UErrorCode err = U_ZERO_ERROR;
            cvt_ = ucnv_open(charset.c_str(), &err);
            if(!cvt_ || U_FAILURE(err))
                throw conv::invalid_charset_error(charset);

            if(cvt_type == cpcvt_type::skip) {
                ucnv_setFromUCallBack(cvt_, UCNV_FROM_U_CALLBACK_SKIP, nullptr, nullptr, nullptr, &err);
                ucnv_setToUCallBack(cvt_, UCNV_TO_U_CALLBACK_SKIP, nullptr, nullptr, nullptr, &err);
                check_and_throw_icu_error(err);
            } else {
                ucnv_setFromUCallBack(cvt_, UCNV_FROM_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &err);
                ucnv_setToUCallBack(cvt_, UCNV_TO_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &err);
                check_and_throw_icu_error(err);
            }
        }

        int max_char_size() const { return ucnv_getMaxCharSize(cvt_); }

        template<typename U8Char = char>
        std::basic_string<U8Char> go(const UChar* buf, int length, int max_size) const
        {
            static_assert(sizeof(U8Char) == sizeof(char), "Not an UTF-8 char type");
            std::basic_string<U8Char> res;
            res.resize(UCNV_GET_MAX_BYTES_FOR_STRING(length, max_size));
            char* ptr = reinterpret_cast<char*>(&res[0]);
            UErrorCode err = U_ZERO_ERROR;
            int n = ucnv_fromUChars(cvt_, ptr, res.size(), buf, length, &err);
            check_and_throw_icu_error(err);
            res.resize(n);
            return res;
        }

        size_t cut(size_t n, const char* begin, const char* end) const
        {
            const char* saved = begin;
            while(n > 0 && begin < end) {
                UErrorCode err = U_ZERO_ERROR;
                ucnv_getNextUChar(cvt_, &begin, end, &err);
                if(U_FAILURE(err))
                    return 0;
                n--;
            }
            return begin - saved;
        }

        UConverter* cvt() const { return cvt_; }

    private:
        icu_handle cvt_;
    };

    template<typename CharType, int char_size = sizeof(CharType)>
    class icu_std_converter;

    template<typename CharType>
    class icu_std_converter<CharType, 1> {
    public:
        typedef std::basic_string<CharType> string_type;

        icu::UnicodeString icu_checked(const CharType* vb, const CharType* ve) const
        {
            return icu(vb, ve); // Already done
        }
        icu::UnicodeString icu(const CharType* vb, const CharType* ve) const
        {
            const char* begin = reinterpret_cast<const char*>(vb);
            const char* end = reinterpret_cast<const char*>(ve);
            UErrorCode err = U_ZERO_ERROR;
            icu::UnicodeString tmp(begin, end - begin, cvt_.cvt(), err);
            check_and_throw_icu_error(err);
            return tmp;
        }

        string_type std(const icu::UnicodeString& str) const
        {
            return cvt_.go<CharType>(str.getBuffer(), str.length(), max_len_);
        }

        icu_std_converter(const std::string& charset, cpcvt_type cvt_type = cpcvt_type::skip) :
            cvt_(charset, cvt_type), max_len_(cvt_.max_char_size())
        {}

        size_t cut(const icu::UnicodeString& str,
                   const CharType* begin,
                   const CharType* end,
                   size_t n,
                   size_t from_u = 0,
                   size_t from_char = 0) const
        {
            size_t code_points = str.countChar32(from_u, n);
            return cvt_.cut(code_points,
                            reinterpret_cast<const char*>(begin) + from_char,
                            reinterpret_cast<const char*>(end));
        }

    private:
        uconv cvt_;
        const int max_len_;
    };

    template<typename CharType>
    class icu_std_converter<CharType, 2> {
    public:
        typedef std::basic_string<CharType> string_type;

        icu::UnicodeString icu_checked(const CharType* begin, const CharType* end) const
        {
            icu::UnicodeString tmp(end - begin, 0, 0); // make initial capacity
            while(begin != end) {
                UChar cl = *begin++;
                if(U16_IS_SINGLE(cl))
                    tmp.append(static_cast<UChar32>(cl));
                else if(U16_IS_LEAD(cl)) {
                    if(begin == end)
                        throw_if_needed();
                    else {
                        UChar ct = *begin++;
                        if(!U16_IS_TRAIL(ct))
                            throw_if_needed();
                        else {
                            UChar32 c = U16_GET_SUPPLEMENTARY(cl, ct);
                            tmp.append(c);
                        }
                    }
                } else
                    throw_if_needed();
            }
            return tmp;
        }
        void throw_if_needed() const
        {
            if(mode_ == cpcvt_type::stop)
                throw conv::conversion_error();
        }
        icu::UnicodeString icu(const CharType* vb, const CharType* ve) const
        {
            static_assert(sizeof(CharType) == sizeof(UChar), "Size mismatch!");
            const UChar* begin = reinterpret_cast<const UChar*>(vb);
            const UChar* end = reinterpret_cast<const UChar*>(ve);
            icu::UnicodeString tmp(begin, end - begin);
            return tmp;
        }

        string_type std(const icu::UnicodeString& str) const
        {
            static_assert(sizeof(CharType) == sizeof(UChar), "Size mismatch!");
            const CharType* ptr = reinterpret_cast<const CharType*>(str.getBuffer());
            return string_type(ptr, str.length());
        }
        size_t cut(const icu::UnicodeString& /*str*/,
                   const CharType* /*begin*/,
                   const CharType* /*end*/,
                   size_t n,
                   size_t /*from_u*/ = 0,
                   size_t /*from_c*/ = 0) const
        {
            return n;
        }

        icu_std_converter(std::string /*charset*/, cpcvt_type mode = cpcvt_type::skip) : mode_(mode) {}

    private:
        cpcvt_type mode_;
    };

    template<typename CharType>
    class icu_std_converter<CharType, 4> {
    public:
        typedef std::basic_string<CharType> string_type;

        icu::UnicodeString icu_checked(const CharType* begin, const CharType* end) const
        {
            // Fast path checking if the full string is already valid
            {
                UErrorCode err = U_ZERO_ERROR;
                static_assert(sizeof(CharType) == sizeof(UChar32), "Size mismatch!");
                u_strFromUTF32(nullptr, 0, nullptr, reinterpret_cast<const UChar32*>(begin), end - begin, &err);
                if(err != U_INVALID_CHAR_FOUND)
                    return icu::UnicodeString::fromUTF32(reinterpret_cast<const UChar32*>(begin), end - begin);
            }
            // Any char is invalid
            throw_if_needed();
            // If not thrown skip invalid chars
            icu::UnicodeString tmp(end - begin, 0, 0); // make initial capacity
            while(begin != end) {
                const UChar32 c = static_cast<UChar32>(*begin++);
                // Maybe simply: UCHAR_MIN_VALUE <= c && c <= UCHAR_MAX_VALUE && !U_IS_SURROGATE(c)
                UErrorCode err = U_ZERO_ERROR;
                u_strFromUTF32(nullptr, 0, nullptr, &c, 1, &err);
                if(err != U_INVALID_CHAR_FOUND)
                    tmp.append(c);
            }
            return tmp;
        }
        void throw_if_needed() const
        {
            if(mode_ == cpcvt_type::stop)
                throw conv::conversion_error();
        }

        icu::UnicodeString icu(const CharType* begin, const CharType* end) const
        {
            icu::UnicodeString tmp(end - begin, 0, 0); // make initial capacity
            while(begin != end) {
                UChar32 c = static_cast<UChar32>(*begin++);
                tmp.append(c);
            }
            return tmp;
        }

        string_type std(const icu::UnicodeString& str) const
        {
            string_type tmp;
            tmp.resize(str.length());
            UChar32* ptr = reinterpret_cast<UChar32*>(&tmp[0]);
            int32_t len = 0;
            UErrorCode code = U_ZERO_ERROR;
            u_strToUTF32(ptr, tmp.size(), &len, str.getBuffer(), str.length(), &code);

            check_and_throw_icu_error(code);

            tmp.resize(len);

            return tmp;
        }

        size_t cut(const icu::UnicodeString& str,
                   const CharType* /*begin*/,
                   const CharType* /*end*/,
                   size_t n,
                   size_t from_u = 0,
                   size_t /*from_c*/ = 0) const
        {
            return str.countChar32(from_u, n);
        }

        icu_std_converter(std::string /*charset*/, cpcvt_type mode = cpcvt_type::skip) : mode_(mode) {}

    private:
        cpcvt_type mode_;
    };
}}} // namespace boost::locale::impl_icu

#endif

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif
