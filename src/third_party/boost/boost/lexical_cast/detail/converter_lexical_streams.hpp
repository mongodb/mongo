// Copyright Kevlin Henney, 2000-2005.
// Copyright Alexander Nasonov, 2006-2010.
// Copyright Antony Polukhin, 2011-2025.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// what:  lexical_cast custom keyword cast
// who:   contributed by Kevlin Henney,
//        enhanced with contributions from Terje Slettebo,
//        with additional fixes and suggestions from Gennaro Prota,
//        Beman Dawes, Dave Abrahams, Daryle Walker, Peter Dimov,
//        Alexander Nasonov, Antony Polukhin, Justin Viiret, Michael Hofmann,
//        Cheng Yang, Matthew Bradbury, David W. Birdsall, Pavel Korzh and other Boosters
// when:  November 2000, March 2003, June 2005, June 2006, March 2011 - 2014, Nowember 2016

#ifndef BOOST_LEXICAL_CAST_DETAIL_CONVERTER_LEXICAL_STREAMS_HPP
#define BOOST_LEXICAL_CAST_DETAIL_CONVERTER_LEXICAL_STREAMS_HPP

#include <boost/config.hpp>
#ifdef BOOST_HAS_PRAGMA_ONCE
#   pragma once
#endif


#if defined(BOOST_NO_STRINGSTREAM) || defined(BOOST_NO_STD_WSTRING)
#define BOOST_LCAST_NO_WCHAR_T
#endif

#include <cstddef>
#include <string>
#include <cstring>
#include <cstdio>
#include <boost/limits.hpp>
#include <boost/type_traits/conditional.hpp>
#include <boost/type_traits/is_enum.hpp>
#include <boost/type_traits/is_signed.hpp>
#include <boost/type_traits/is_unsigned.hpp>
#include <boost/type_traits/is_pointer.hpp>
#include <boost/detail/lcast_precision.hpp>
#include <boost/config/workaround.hpp>
#include <boost/core/snprintf.hpp>

#ifndef BOOST_NO_STD_LOCALE
#   include <locale>
#else
#   ifndef BOOST_LEXICAL_CAST_ASSUME_C_LOCALE
        // Getting error at this point means, that your STL library is old/lame/misconfigured.
        // If nothing can be done with STL library, define BOOST_LEXICAL_CAST_ASSUME_C_LOCALE,
        // but beware: lexical_cast will understand only 'C' locale delimeters and thousands
        // separators.
#       error "Unable to use <locale> header. Define BOOST_LEXICAL_CAST_ASSUME_C_LOCALE to force "
#       error "boost::lexical_cast to use only 'C' locale during conversions."
#   endif
#endif

#ifdef BOOST_NO_STRINGSTREAM
#include <strstream>
#else
#include <sstream>
#endif

#include <boost/lexical_cast/detail/buffer_view.hpp>
#include <boost/lexical_cast/detail/lcast_char_constants.hpp>
#include <boost/lexical_cast/detail/lcast_unsigned_converters.hpp>
#include <boost/lexical_cast/detail/lcast_basic_unlockedbuf.hpp>
#include <boost/lexical_cast/detail/inf_nan.hpp>

#include <istream>

#include <array>

#include <boost/type_traits/make_unsigned.hpp>
#include <boost/type_traits/is_integral.hpp>
#include <boost/type_traits/is_float.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/type_traits/is_reference.hpp>
#include <boost/container/container_fwd.hpp>
#include <boost/core/enable_if.hpp>
#ifndef BOOST_NO_CWCHAR
#   include <cwchar>
#endif

// Forward declarations
namespace boost {
    template<class T, std::size_t N>
    class array;
    template<class IteratorT>
    class iterator_range;

    // forward declaration of boost::basic_string_view from Utility
    template<class Ch, class Tr> class basic_string_view;
}

namespace boost { namespace detail { namespace lcast {

    template <typename T>
    struct exact {
        static_assert(!boost::is_const<T>::value, "");
        static_assert(!boost::is_reference<T>::value, "");

        const T& payload;
    };

    template< class CharT // a result of widest_char transformation
            , class Traits
            , std::size_t CharacterBufferSize
            >
    class optimized_src_stream {
        CharT buffer[CharacterBufferSize];

        // After the `stream_in(`  finishes, `[start, finish)` is
        // the range to output by `operator >>`
        const CharT*  start;
        const CharT*  finish;
    public:
        optimized_src_stream(optimized_src_stream&&) = delete;
        optimized_src_stream(const optimized_src_stream&) = delete;
        optimized_src_stream& operator=(optimized_src_stream&&) = delete;
        optimized_src_stream& operator=(const optimized_src_stream&) = delete;

        optimized_src_stream() noexcept
          : start(buffer)
          , finish(buffer + CharacterBufferSize)
        {}

        const CharT* cbegin() const noexcept {
            return start;
        }

        const CharT* cend() const noexcept {
            return finish;
        }

    private:
        bool shl_char(CharT ch) noexcept {
            Traits::assign(buffer[0], ch);
            finish = start + 1;
            return true;
        }

#ifndef BOOST_LCAST_NO_WCHAR_T
        template <class T>
        bool shl_char(T ch) {
            static_assert(sizeof(T) <= sizeof(CharT),
                "boost::lexical_cast does not support narrowing of char types."
                "Use boost::locale instead" );
#ifndef BOOST_LEXICAL_CAST_ASSUME_C_LOCALE
            std::locale loc;
            CharT const w = BOOST_USE_FACET(std::ctype<CharT>, loc).widen(ch);
#else
            CharT const w = static_cast<CharT>(ch);
#endif
            Traits::assign(buffer[0], w);
            finish = start + 1;
            return true;
        }
#endif

        bool shl_char_array(CharT const* str_value) noexcept {
            start = str_value;
            finish = start + Traits::length(str_value);
            return true;
        }

        bool shl_char_array_limited(CharT const* str, std::size_t max_size) noexcept {
            start = str;
            finish = start;
            const auto zero = Traits::to_char_type(0);
            while (finish < start + max_size && zero != *finish) {
                 ++ finish;
            }
            return true;
        }

        template <class T>
        inline bool shl_unsigned(const T n) {
            CharT* tmp_finish = buffer + CharacterBufferSize;
            start = lcast_put_unsigned<Traits, T, CharT>(n, tmp_finish).convert();
            finish = tmp_finish;
            return true;
        }

        template <class T>
        inline bool shl_signed(const T n) {
            CharT* tmp_finish = buffer + CharacterBufferSize;
            typedef typename boost::make_unsigned<T>::type utype;
            CharT* tmp_start = lcast_put_unsigned<Traits, utype, CharT>(lcast_to_unsigned(n), tmp_finish).convert();
            if (n < 0) {
                --tmp_start;
                CharT const minus = lcast_char_constants<CharT>::minus;
                Traits::assign(*tmp_start, minus);
            }
            start = tmp_start;
            finish = tmp_finish;
            return true;
        }

        bool shl_real_type(float val, char* begin) {
            const double val_as_double = val;
            finish = start +
                boost::core::snprintf(begin, CharacterBufferSize,
                "%.*g", static_cast<int>(boost::detail::lcast_precision<float>::value), val_as_double);
            return finish > start;
        }

        bool shl_real_type(double val, char* begin) {
            finish = start +
                boost::core::snprintf(begin, CharacterBufferSize,
                "%.*g", static_cast<int>(boost::detail::lcast_precision<double>::value), val);
            return finish > start;
        }

#ifndef __MINGW32__
        bool shl_real_type(long double val, char* begin) {
            finish = start +
                boost::core::snprintf(begin, CharacterBufferSize,
                "%.*Lg", static_cast<int>(boost::detail::lcast_precision<long double>::value), val );
            return finish > start;
        }
#else
        bool shl_real_type(long double val, char* begin) {
            return shl_real_type(static_cast<double>(val), begin);
        }
#endif

#if !defined(BOOST_LCAST_NO_WCHAR_T)
        bool shl_real_type(float val, wchar_t* begin) {
            const double val_as_double = val;
            finish = start + boost::core::swprintf(
                begin, CharacterBufferSize, L"%.*g",
                static_cast<int>(boost::detail::lcast_precision<float>::value),
                val_as_double
            );
            return finish > start;
        }

        bool shl_real_type(double val, wchar_t* begin) {
            finish = start + boost::core::swprintf(
              begin, CharacterBufferSize, L"%.*g",
              static_cast<int>(boost::detail::lcast_precision<double>::value),
              val
            );
            return finish > start;
        }

        bool shl_real_type(long double val, wchar_t* begin) {
            finish = start + boost::core::swprintf(
                begin, CharacterBufferSize, L"%.*Lg",
                static_cast<int>(boost::detail::lcast_precision<long double>::value),
                val
            );
            return finish > start;
        }
#endif
    public:
        template <class C>
        using enable_if_compatible_char_t = typename boost::enable_if_c<
            boost::is_same<const C, const CharT>::value || (
                boost::is_same<const char, const CharT>::value && (
                    boost::is_same<const C, const unsigned char>::value ||
                    boost::is_same<const C, const signed char>::value
                )
            ), bool
        >::type;

        template<class CharTraits, class Alloc>
        bool stream_in(lcast::exact<std::basic_string<CharT,CharTraits,Alloc>> x) noexcept {
            start = x.payload.data();
            finish = start + x.payload.length();
            return true;
        }

        template<class CharTraits, class Alloc>
        bool stream_in(lcast::exact<boost::container::basic_string<CharT,CharTraits,Alloc>> x) noexcept {
            start = x.payload.data();
            finish = start + x.payload.length();
            return true;
        }

        bool stream_in(lcast::exact<bool> x) noexcept {
            CharT const czero = lcast_char_constants<CharT>::zero;
            Traits::assign(buffer[0], Traits::to_char_type(czero + x.payload));
            finish = start + 1;
            return true;
        }

        bool stream_in(lcast::exact<boost::conversion::detail::buffer_view<CharT>> x) noexcept {
            start = x.payload.begin;
            finish = x.payload.end;
            return true;
        }

        template <class C>
        enable_if_compatible_char_t<C>
        stream_in(lcast::exact<boost::iterator_range<C*>> x) noexcept {
            auto buf = boost::conversion::detail::make_buffer_view(x.payload.begin(), x.payload.end());
            return stream_in(lcast::exact<decltype(buf)>{buf});
        }

        bool stream_in(lcast::exact<char> x)                    { return shl_char(x.payload); }
        bool stream_in(lcast::exact<unsigned char> x)           { return shl_char(static_cast<char>(x.payload)); }
        bool stream_in(lcast::exact<signed char> x)             { return shl_char(static_cast<char>(x.payload)); }
        
        template <class C>
        typename boost::enable_if_c<boost::detail::is_character<C>::value, bool>::type
                stream_in(lcast::exact<C> x)                    { return shl_char(x.payload); }

        template <class Type>
        enable_if_compatible_char_t<Type>
                stream_in(lcast::exact<Type*> x)                { return shl_char_array(reinterpret_cast<CharT const*>(x.payload)); }

        template <class Type>
        typename boost::enable_if_c<boost::is_signed<Type>::value && !boost::is_enum<Type>::value, bool>::type
                stream_in(lcast::exact<Type> x)                  { return shl_signed(x.payload); }

        template <class Type>
        typename boost::enable_if_c<boost::is_unsigned<Type>::value && !boost::is_enum<Type>::value, bool>::type
                stream_in(lcast::exact<Type> x)                  { return shl_unsigned(x.payload); }

        template <class Type>
        auto stream_in(lcast::exact<Type> x) -> decltype(shl_real_type(x.payload, buffer)) {
            const CharT* inf_nan  = detail::get_inf_nan(x.payload, CharT());
            if (inf_nan) {
                start = inf_nan;
                finish = start + Traits::length(inf_nan);
                return true;
            }
            return shl_real_type(x.payload, buffer);
        }

        template <class C, std::size_t N>
        enable_if_compatible_char_t<C>
        stream_in(lcast::exact<boost::array<C, N>> x) noexcept {
            return shl_char_array_limited(reinterpret_cast<const CharT*>(x.payload.data()), N);
        }

        template <class C, std::size_t N>
        enable_if_compatible_char_t<C>
        stream_in(lcast::exact<std::array<C, N>> x) noexcept {
            return shl_char_array_limited(reinterpret_cast<const CharT*>(x.payload.data()), N);
        }

#ifndef BOOST_NO_CXX17_HDR_STRING_VIEW
        template <class C, class CharTraits>
        enable_if_compatible_char_t<C>
        stream_in(lcast::exact<std::basic_string_view<C, CharTraits>> x) noexcept {
            start = reinterpret_cast<const CharT*>(x.payload.data());
            finish = start + x.payload.size();
            return true;
        }
#endif
        template <class C, class CharTraits>
        enable_if_compatible_char_t<C>
        stream_in(lcast::exact<boost::basic_string_view<C, CharTraits>> x) noexcept {
            start = reinterpret_cast<const CharT*>(x.payload.data());
            finish = start + x.payload.size();
            return true;
        }
    };


    template <class CharT, class Traits>
    class ios_src_stream {
        typedef detail::lcast::out_stream_t<CharT, Traits> deduced_out_stream_t;
        typedef detail::lcast::stringbuffer_t<CharT, Traits> deduced_out_buffer_t;

        deduced_out_buffer_t out_buffer;
        deduced_out_stream_t out_stream;

        const CharT*  start = nullptr;
        const CharT*  finish = nullptr;
    public:
        ios_src_stream(ios_src_stream&&) = delete;
        ios_src_stream(const ios_src_stream&) = delete;
        ios_src_stream& operator=(ios_src_stream&&) = delete;
        ios_src_stream& operator=(const ios_src_stream&) = delete;

        ios_src_stream(): out_buffer(), out_stream(&out_buffer) {}

        const CharT* cbegin() const noexcept {
            return start;
        }

        const CharT* cend() const noexcept {
            return finish;
        }
    private:
        const deduced_out_buffer_t* get_rdbuf() const {
            return static_cast<deduced_out_buffer_t*>(
                out_stream.rdbuf()
            );
        }

        template<typename InputStreamable>
        bool shl_input_streamable(InputStreamable& input) {
#if defined(BOOST_NO_STRINGSTREAM) || defined(BOOST_NO_STD_LOCALE)
            // If you have compilation error at this point, than your STL library
            // does not support such conversions. Try updating it.
            static_assert(boost::is_same<char, CharT>::value, "");
#endif

#ifndef BOOST_NO_EXCEPTIONS
            out_stream.exceptions(std::ios::badbit);
            try {
#endif
            bool const result = !(out_stream << input).fail();
            const auto* const p = get_rdbuf();
            start = p->pbase();
            finish = p->pptr();
            return result;
#ifndef BOOST_NO_EXCEPTIONS
            } catch (const ::std::ios_base::failure& /*f*/) {
                return false;
            }
#endif
        }

        template <class T>
        bool shl_char_array(T const* str_value) {
            static_assert(sizeof(T) <= sizeof(CharT),
                "boost::lexical_cast does not support narrowing of char types."
                "Use boost::locale instead" );
            return shl_input_streamable(str_value);
        }

        template <class T>
        bool shl_real(T val) {
            const CharT* inf_nan  = detail::get_inf_nan(val, CharT());
            if (inf_nan) {
                start = inf_nan;
                finish = start + Traits::length(inf_nan);
                return true;
            }

            lcast_set_precision(out_stream, &val);
            return shl_input_streamable(val);
        }

    public:
        template <class Type>
        typename boost::enable_if_c<boost::detail::is_character<Type>::value && sizeof(char) == sizeof(Type), bool>::type
                stream_in(lcast::exact<const Type*> x)         { return shl_char_array(reinterpret_cast<char const*>(x.payload)); }

        template <class Type>
        typename boost::enable_if_c<boost::detail::is_character<Type>::value && sizeof(char) != sizeof(Type), bool>::type
                stream_in(lcast::exact<const Type*> x)         { return shl_char_array(x.payload); }

        bool stream_in(lcast::exact<float> x)                  { return shl_real(x.payload); }
        bool stream_in(lcast::exact<double> x)                 { return shl_real(x.payload); }
        bool stream_in(lcast::exact<long double> x)            {
#ifndef __MINGW32__
            return shl_real(x.payload);
#else
            return shl_real(static_cast<double>(x.payload));
#endif
        }

        template <class C>
        typename boost::enable_if_c<boost::detail::is_character<C>::value, bool>::type
        stream_in(lcast::exact<iterator_range<C*>> x) noexcept {
            auto buf = boost::conversion::detail::make_buffer_view(x.payload.begin(), x.payload.end());
            return stream_in(lcast::exact<decltype(buf)>{buf});
        }

        template <class C>
        typename boost::enable_if_c<boost::detail::is_character<C>::value, bool>::type
        stream_in(lcast::exact<iterator_range<const C*>> x) noexcept {
            auto buf = boost::conversion::detail::make_buffer_view(x.payload.begin(), x.payload.end());
            return stream_in(lcast::exact<decltype(buf)>{buf});
        }

        template <class InStreamable>
        bool stream_in(lcast::exact<InStreamable> x)  { return shl_input_streamable(x.payload); }
    };


    template <class CharT, class Traits>
    class to_target_stream {
        //`[start, finish)` is the range to output by `operator >>`
        const CharT*        start;
        const CharT* const  finish;

    public:
        to_target_stream(to_target_stream&&) = delete;
        to_target_stream(const to_target_stream&) = delete;
        to_target_stream& operator=(to_target_stream&&) = delete;
        to_target_stream& operator=(const to_target_stream&) = delete;

        to_target_stream(const CharT* begin, const CharT* end) noexcept
          : start(begin)
          , finish(end)
        {}

    private:
        template <typename Type>
#if defined(__clang__) && (__clang_major__ > 3 || __clang_minor__ > 6)
        __attribute__((no_sanitize("unsigned-integer-overflow")))
#endif
        bool shr_unsigned(Type& output) {
            if (start == finish) return false;
            CharT const minus = lcast_char_constants<CharT>::minus;
            CharT const plus = lcast_char_constants<CharT>::plus;
            bool const has_minus = Traits::eq(minus, *start);

            /* We won`t use `start' any more, so no need in decrementing it after */
            if (has_minus || Traits::eq(plus, *start)) {
                ++start;
            }

            bool const succeed = lcast_ret_unsigned<Traits, Type, CharT>(output, start, finish).convert();

            if (has_minus) {
                output = static_cast<Type>(0u - output);
            }

            return succeed;
        }

        template <typename Type>
#if defined(__clang__) && (__clang_major__ > 3 || __clang_minor__ > 6)
        __attribute__((no_sanitize("unsigned-integer-overflow")))
#endif
        bool shr_signed(Type& output) {
            if (start == finish) return false;
            CharT const minus = lcast_char_constants<CharT>::minus;
            CharT const plus = lcast_char_constants<CharT>::plus;
            typedef typename make_unsigned<Type>::type utype;
            utype out_tmp = 0;
            bool const has_minus = Traits::eq(minus, *start);

            /* We won`t use `start' any more, so no need in decrementing it after */
            if (has_minus || Traits::eq(plus, *start)) {
                ++start;
            }

            bool succeed = lcast_ret_unsigned<Traits, utype, CharT>(out_tmp, start, finish).convert();
            if (has_minus) {
                utype const comp_val = (static_cast<utype>(1) << std::numeric_limits<Type>::digits);
                succeed = succeed && out_tmp<=comp_val;
                output = static_cast<Type>(0u - out_tmp);
            } else {
                utype const comp_val = static_cast<utype>((std::numeric_limits<Type>::max)());
                succeed = succeed && out_tmp<=comp_val;
                output = static_cast<Type>(out_tmp);
            }
            return succeed;
        }

        template<typename InputStreamable>
        bool shr_using_base_class(InputStreamable& output)
        {
            static_assert(
                !boost::is_pointer<InputStreamable>::value,
                "boost::lexical_cast can not convert to pointers"
            );

#if defined(BOOST_NO_STRINGSTREAM) || defined(BOOST_NO_STD_LOCALE)
            static_assert(boost::is_same<char, CharT>::value,
                "boost::lexical_cast can not convert, because your STL library does not "
                "support such conversions. Try updating it."
            );
#endif

#if defined(BOOST_NO_STRINGSTREAM)
            std::istrstream stream(start, static_cast<std::istrstream::streamsize>(finish - start));
#else
            typedef detail::lcast::buffer_t<CharT, Traits> buffer_t;
            buffer_t buf;
            // Usually `istream` and `basic_istream` do not modify
            // content of buffer; `buffer_t` assures that this is true
            buf.setbuf(const_cast<CharT*>(start), static_cast<typename buffer_t::streamsize>(finish - start));
#if defined(BOOST_NO_STD_LOCALE)
            std::istream stream(&buf);
#else
            std::basic_istream<CharT, Traits> stream(&buf);
#endif // BOOST_NO_STD_LOCALE
#endif // BOOST_NO_STRINGSTREAM

#ifndef BOOST_NO_EXCEPTIONS
            stream.exceptions(std::ios::badbit);
            try {
#endif
            stream.unsetf(std::ios::skipws);
            lcast_set_precision(stream, static_cast<InputStreamable*>(0));

            return (stream >> output)
                && (stream.get() == Traits::eof());

#ifndef BOOST_NO_EXCEPTIONS
            } catch (const ::std::ios_base::failure& /*f*/) {
                return false;
            }
#endif
        }

        template<class T>
        inline bool shr_xchar(T& output) noexcept {
            static_assert(sizeof(CharT) == sizeof(T),
                "boost::lexical_cast does not support narrowing of character types."
                "Use boost::locale instead" );
            bool const ok = (finish - start == 1);
            if (ok) {
                CharT out;
                Traits::assign(out, *start);
                output = static_cast<T>(out);
            }
            return ok;
        }

        template <std::size_t N, class ArrayT>
        bool shr_std_array(ArrayT& output) noexcept {
            const std::size_t size = static_cast<std::size_t>(finish - start);
            if (size > N - 1) { // `-1` because we need to store \0 at the end
                return false;
            }

            std::memcpy(&output[0], start, size * sizeof(CharT));
            output[size] = Traits::to_char_type(0);
            return true;
        }

    public:
        bool stream_out(unsigned short& output)             { return shr_unsigned(output); }
        bool stream_out(unsigned int& output)               { return shr_unsigned(output); }
        bool stream_out(unsigned long int& output)          { return shr_unsigned(output); }
        bool stream_out(short& output)                      { return shr_signed(output); }
        bool stream_out(int& output)                        { return shr_signed(output); }
        bool stream_out(long int& output)                   { return shr_signed(output); }
#if defined(BOOST_HAS_LONG_LONG)
        bool stream_out(boost::ulong_long_type& output)     { return shr_unsigned(output); }
        bool stream_out(boost::long_long_type& output)      { return shr_signed(output); }
#elif defined(BOOST_HAS_MS_INT64)
        bool stream_out(unsigned __int64& output)           { return shr_unsigned(output); }
        bool stream_out(__int64& output)                    { return shr_signed(output); }
#endif

#ifdef BOOST_HAS_INT128
        bool stream_out(boost::uint128_type& output)        { return shr_unsigned(output); }
        bool stream_out(boost::int128_type& output)         { return shr_signed(output); }
#endif

        bool stream_out(char& output)                       { return shr_xchar(output); }
        bool stream_out(unsigned char& output)              { return shr_xchar(output); }
        bool stream_out(signed char& output)                { return shr_xchar(output); }
#if !defined(BOOST_LCAST_NO_WCHAR_T) && !defined(BOOST_NO_INTRINSIC_WCHAR_T)
        bool stream_out(wchar_t& output)                    { return shr_xchar(output); }
#endif
#if !defined(BOOST_NO_CXX11_CHAR16_T) && !defined(BOOST_NO_CXX11_UNICODE_LITERALS)
        bool stream_out(char16_t& output)                   { return shr_xchar(output); }
#endif
#if !defined(BOOST_NO_CXX11_CHAR32_T) && !defined(BOOST_NO_CXX11_UNICODE_LITERALS)
        bool stream_out(char32_t& output)                   { return shr_xchar(output); }
#endif
        template<class CharTraits, class Alloc>
        bool stream_out(std::basic_string<CharT,CharTraits,Alloc>& str) {
            str.assign(start, finish); return true;
        }

        template<class CharTraits, class Alloc>
        bool stream_out(boost::container::basic_string<CharT,CharTraits,Alloc>& str) {
            str.assign(start, finish); return true;
        }

        template <class C, std::size_t N>
        bool stream_out(std::array<C, N>& output) noexcept {
            static_assert(sizeof(C) == sizeof(CharT), "");
            return shr_std_array<N>(output);
        }

        template <class C, std::size_t N>
        bool stream_out(boost::array<C, N>& output) noexcept {
            static_assert(sizeof(C) == sizeof(CharT), "");
            return shr_std_array<N>(output);
        }

        bool stream_out(bool& output) noexcept {
            output = false; // Suppress warning about uninitalized variable

            if (start == finish) return false;
            CharT const zero = lcast_char_constants<CharT>::zero;
            CharT const plus = lcast_char_constants<CharT>::plus;
            CharT const minus = lcast_char_constants<CharT>::minus;

            const CharT* const dec_finish = finish - 1;
            output = Traits::eq(*dec_finish, zero + 1);
            if (!output && !Traits::eq(*dec_finish, zero)) {
                return false; // Does not ends on '0' or '1'
            }

            if (start == dec_finish) return true;

            // We may have sign at the beginning
            if (Traits::eq(plus, *start) || (Traits::eq(minus, *start) && !output)) {
                ++ start;
            }

            // Skipping zeros
            while (start != dec_finish) {
                if (!Traits::eq(zero, *start)) {
                    return false; // Not a zero => error
                }

                ++ start;
            }

            return true;
        }

    private:
        // Not optimised converter
        template <class T>
        bool float_types_converter_internal(T& output) {
            if (parse_inf_nan(start, finish, output)) return true;
            bool const return_value = shr_using_base_class(output);

            /* Some compilers and libraries successfully
             * parse 'inf', 'INFINITY', '1.0E', '1.0E-'...
             * We are trying to provide a unified behaviour,
             * so we just forbid such conversions (as some
             * of the most popular compilers/libraries do)
             * */
            CharT const minus = lcast_char_constants<CharT>::minus;
            CharT const plus = lcast_char_constants<CharT>::plus;
            CharT const capital_e = lcast_char_constants<CharT>::capital_e;
            CharT const lowercase_e = lcast_char_constants<CharT>::lowercase_e;
            if ( return_value &&
                 (
                    Traits::eq(*(finish-1), lowercase_e)                   // 1.0e
                    || Traits::eq(*(finish-1), capital_e)                  // 1.0E
                    || Traits::eq(*(finish-1), minus)                      // 1.0e- or 1.0E-
                    || Traits::eq(*(finish-1), plus)                       // 1.0e+ or 1.0E+
                 )
            ) return false;

            return return_value;
        }

    public:
        bool stream_out(float& output) { return float_types_converter_internal(output); }
        bool stream_out(double& output) { return float_types_converter_internal(output); }
        bool stream_out(long double& output) { return float_types_converter_internal(output); }

        // Generic istream-based algorithm.
        // lcast_streambuf_for_target<InputStreamable>::value is true.
        template <typename InputStreamable>
        bool stream_out(InputStreamable& output) {
            return shr_using_base_class(output);
        }
    };
    
}}} // namespace boost::detail::lcast

#undef BOOST_LCAST_NO_WCHAR_T

#endif // BOOST_LEXICAL_CAST_DETAIL_CONVERTER_LEXICAL_HPP

