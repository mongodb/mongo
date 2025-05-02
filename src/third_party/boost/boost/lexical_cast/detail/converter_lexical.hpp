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
// when:  November 2000, March 2003, June 2005, June 2006, March 2011 - 2014

#ifndef BOOST_LEXICAL_CAST_DETAIL_CONVERTER_LEXICAL_HPP
#define BOOST_LEXICAL_CAST_DETAIL_CONVERTER_LEXICAL_HPP

#include <boost/config.hpp>
#ifdef BOOST_HAS_PRAGMA_ONCE
#   pragma once
#endif

#if defined(BOOST_NO_STRINGSTREAM) || defined(BOOST_NO_STD_WSTRING)
#define BOOST_LCAST_NO_WCHAR_T
#endif

#include <cstddef>
#include <string>
#include <boost/limits.hpp>
#include <boost/type_traits/integral_constant.hpp>
#include <boost/type_traits/type_identity.hpp>
#include <boost/type_traits/conditional.hpp>
#include <boost/type_traits/is_integral.hpp>
#include <boost/type_traits/is_float.hpp>
#include <boost/detail/lcast_precision.hpp>

#include <boost/lexical_cast/detail/widest_char.hpp>
#include <boost/lexical_cast/detail/is_character.hpp>

#include <array>

#ifndef BOOST_NO_CXX17_HDR_STRING_VIEW
#include <string_view>
#endif

#include <boost/lexical_cast/detail/buffer_view.hpp>
#include <boost/container/container_fwd.hpp>

#include <boost/lexical_cast/detail/converter_lexical_streams.hpp>

namespace boost {

    // Forward declaration
    template<class T, std::size_t N>
    class array;
    template<class IteratorT>
    class iterator_range;

    // Forward declaration of boost::basic_string_view from Utility
    template<class Ch, class Tr> class basic_string_view;

    namespace detail // normalize_single_byte_char<Char>
    {
        // Converts signed/unsigned char to char
        template < class Char >
        struct normalize_single_byte_char
        {
            typedef Char type;
        };

        template <>
        struct normalize_single_byte_char< signed char >
        {
            typedef char type;
        };

        template <>
        struct normalize_single_byte_char< unsigned char >
        {
            typedef char type;
        };
    }

    namespace detail // deduce_character_type_later<T>
    {
        // Helper type, meaning that stram character for T must be deduced
        // at Stage 2 (See deduce_source_char<T> and deduce_target_char<T>)
        template < class T > struct deduce_character_type_later {};
    }

    namespace detail // stream_char_common<T>
    {
        // Selectors to choose stream character type (common for Source and Target)
        // Returns one of char, wchar_t, char16_t, char32_t or deduce_character_type_later<T> types
        // Executed on Stage 1 (See deduce_source_char<T> and deduce_target_char<T>)
        template < typename Type >
        struct stream_char_common: public boost::conditional<
            boost::detail::is_character< Type >::value,
            Type,
            boost::detail::deduce_character_type_later< Type >
        > {};

        template < typename Char >
        struct stream_char_common< Char* >: public boost::conditional<
            boost::detail::is_character< Char >::value,
            Char,
            boost::detail::deduce_character_type_later< Char* >
        > {};

        template < typename Char >
        struct stream_char_common< const Char* >: public boost::conditional<
            boost::detail::is_character< Char >::value,
            Char,
            boost::detail::deduce_character_type_later< const Char* >
        > {};

        template < typename Char >
        struct stream_char_common< boost::conversion::detail::buffer_view< Char > >
        {
            typedef Char type;
        };

        template < typename Char >
        struct stream_char_common< boost::iterator_range< Char* > >: public boost::conditional<
            boost::detail::is_character< Char >::value,
            Char,
            boost::detail::deduce_character_type_later< boost::iterator_range< Char* > >
        > {};

        template < typename Char >
        struct stream_char_common< boost::iterator_range< const Char* > >: public boost::conditional<
            boost::detail::is_character< Char >::value,
            Char,
            boost::detail::deduce_character_type_later< boost::iterator_range< const Char* > >
        > {};

        template < class Char, class Traits, class Alloc >
        struct stream_char_common< std::basic_string< Char, Traits, Alloc > >
        {
            typedef Char type;
        };

        template < class Char, class Traits, class Alloc >
        struct stream_char_common< boost::container::basic_string< Char, Traits, Alloc > >
        {
            typedef Char type;
        };

        template < typename Char, std::size_t N >
        struct stream_char_common< boost::array< Char, N > >: public boost::conditional<
            boost::detail::is_character< Char >::value,
            Char,
            boost::detail::deduce_character_type_later< boost::array< Char, N > >
        > {};

        template < typename Char, std::size_t N >
        struct stream_char_common< boost::array< const Char, N > >: public boost::conditional<
            boost::detail::is_character< Char >::value,
            Char,
            boost::detail::deduce_character_type_later< boost::array< const Char, N > >
        > {};

#ifndef BOOST_NO_CXX11_HDR_ARRAY
        template < typename Char, std::size_t N >
        struct stream_char_common< std::array<Char, N > >: public boost::conditional<
            boost::detail::is_character< Char >::value,
            Char,
            boost::detail::deduce_character_type_later< std::array< Char, N > >
        > {};

        template < typename Char, std::size_t N >
        struct stream_char_common< std::array< const Char, N > >: public boost::conditional<
            boost::detail::is_character< Char >::value,
            Char,
            boost::detail::deduce_character_type_later< std::array< const Char, N > >
        > {};
#endif

#ifndef BOOST_NO_CXX17_HDR_STRING_VIEW
        template < class Char, class Traits >
        struct stream_char_common< std::basic_string_view< Char, Traits > >
        {
            typedef Char type;
        };
#endif
        template < class Char, class Traits >
        struct stream_char_common< boost::basic_string_view< Char, Traits > >
        {
            typedef Char type;
        };

#ifdef BOOST_HAS_INT128
        template <> struct stream_char_common< boost::int128_type >: public boost::type_identity< char > {};
        template <> struct stream_char_common< boost::uint128_type >: public boost::type_identity< char > {};
#endif

#if !defined(BOOST_LCAST_NO_WCHAR_T) && defined(BOOST_NO_INTRINSIC_WCHAR_T)
        template <>
        struct stream_char_common< wchar_t >
        {
            typedef char type;
        };
#endif
    }

    namespace detail // deduce_source_char_impl<T>
    {
        // If type T is `deduce_character_type_later` type, then tries to deduce
        // character type using streaming metafunctions.
        // Otherwise supplied type T is a character type, that must be normalized
        // using normalize_single_byte_char<Char>.
        // Executed at Stage 2  (See deduce_source_char<T> and deduce_target_char<T>)
        template < class Char >
        struct deduce_source_char_impl
        {
            typedef typename boost::detail::normalize_single_byte_char< Char >::type type;
        };

        template < class T >
        struct deduce_source_char_impl< deduce_character_type_later< T > >
        {
            template <class U>
            static auto left_shift_type(long)
                -> decltype( std::declval<std::basic_ostream< char >&>() << std::declval<const U&>(), char{});

#if !defined(BOOST_LCAST_NO_WCHAR_T)
            template <class U>
            static auto left_shift_type(int)
                -> decltype( std::declval<std::basic_ostream< wchar_t >&>() << std::declval<const U&>(), wchar_t{});
#endif

            template <class U>
            static void left_shift_type(...);

            using type = decltype(left_shift_type<T>(1L));

            static_assert(!std::is_same<type, void>::value,
#if defined(BOOST_LCAST_NO_WCHAR_T)
                "Source type is not std::ostream`able and std::wostream`s are "
                "not supported by your STL implementation"
#else
                "Source type is neither std::ostream`able nor std::wostream`able"
#endif
            );
        };
    }

    namespace detail  // deduce_target_char_impl<T>
    {
        // If type T is `deduce_character_type_later` type, then tries to deduce
        // character type using boost::has_right_shift<T> metafunction.
        // Otherwise supplied type T is a character type, that must be normalized
        // using normalize_single_byte_char<Char>.
        // Executed at Stage 2  (See deduce_source_char<T> and deduce_target_char<T>)
        template < class Char >
        struct deduce_target_char_impl
        {
            typedef typename normalize_single_byte_char< Char >::type type;
        };

        template < class T >
        struct deduce_target_char_impl< deduce_character_type_later<T> >
        {
            template <class U>
            static auto right_shift_type(long)
                -> decltype( std::declval<std::basic_istream< char >&>() >> std::declval<U&>(), char{});

#if !defined(BOOST_LCAST_NO_WCHAR_T)
            template <class U>
            static auto right_shift_type(int)
                -> decltype( std::declval<std::basic_istream< wchar_t >&>() >> std::declval<U&>(), wchar_t{});
#endif

            template <class U>
            static void right_shift_type(...);

            using type = decltype(right_shift_type<T>(1L));

            static_assert(!std::is_same<type, void>::value,
#if defined(BOOST_LCAST_NO_WCHAR_T)
               "Target type is not std::istream`able and std::wistream`s are "
               "not supported by your STL implementation"
#else
               "Target type is neither std::istream`able nor std::wistream`able"
#endif
            );
        };
    }

    namespace detail  // deduce_target_char<T> and deduce_source_char<T>
    {
        // We deduce stream character types in two stages.
        //
        // Stage 1 is common for Target and Source. At Stage 1 we get
        // non normalized character type (may contain unsigned/signed char)
        // or deduce_character_type_later<T> where T is the original type.
        // Stage 1 is executed by stream_char_common<T>
        //
        // At Stage 2 we normalize character types or try to deduce character
        // type using metafunctions.
        // Stage 2 is executed by deduce_target_char_impl<T> and
        // deduce_source_char_impl<T>
        //
        // deduce_target_char<T> and deduce_source_char<T> functions combine
        // both stages

        template < class T >
        struct deduce_target_char
        {
            typedef typename stream_char_common< T >::type stage1_type;
            typedef typename deduce_target_char_impl< stage1_type >::type type;
        };

        template < class T >
        struct deduce_source_char
        {
            typedef typename stream_char_common< T >::type stage1_type;
            typedef typename deduce_source_char_impl< stage1_type >::type type;
        };
    }

    namespace detail // array_to_pointer_decay<T>
    {
        template<class T>
        struct array_to_pointer_decay
        {
            typedef T type;
        };

        template<class T, std::size_t N>
        struct array_to_pointer_decay<T[N]>
        {
            typedef const T * type;
        };
    }

    namespace detail // lcast_src_length
    {
        // Return max. length of string representation of Source;
        template< class Source,         // Source type of lexical_cast.
                  class Enable = void   // helper type
                >
        struct lcast_src_length
        {
            BOOST_STATIC_CONSTANT(std::size_t, value = 1);
        };

        // Helper for integral types.
        // Notes on length calculation:
        // Max length for 32bit int with grouping "\1" and thousands_sep ',':
        // "-2,1,4,7,4,8,3,6,4,7"
        //  ^                    - is_signed
        //   ^                   - 1 digit not counted by digits10
        //    ^^^^^^^^^^^^^^^^^^ - digits10 * 2
        //
        // Constant is_specialized is used instead of constant 1
        // to prevent buffer overflow in a rare case when
        // <boost/limits.hpp> doesn't add missing specialization for
        // numeric_limits<T> for some integral type T.
        // When is_specialized is false, the whole expression is 0.
        template <class Source>
        struct lcast_src_length<
                    Source, typename boost::enable_if<boost::is_integral<Source> >::type
                >
        {
            BOOST_STATIC_CONSTANT(std::size_t, value =
                  std::numeric_limits<Source>::is_signed +
                  std::numeric_limits<Source>::is_specialized + /* == 1 */
                  std::numeric_limits<Source>::digits10 * 2
              );
        };

        // Helper for floating point types.
        // -1.23456789e-123456
        // ^                   sign
        //  ^                  leading digit
        //   ^                 decimal point
        //    ^^^^^^^^         lcast_precision<Source>::value
        //            ^        "e"
        //             ^       exponent sign
        //              ^^^^^^ exponent (assumed 6 or less digits)
        // sign + leading digit + decimal point + "e" + exponent sign == 5
        template<class Source>
        struct lcast_src_length<
                Source, typename boost::enable_if<boost::is_float<Source> >::type
            >
        {
            static_assert(
                    std::numeric_limits<Source>::max_exponent10 <=  999999L &&
                    std::numeric_limits<Source>::min_exponent10 >= -999999L
                , "");

            BOOST_STATIC_CONSTANT(std::size_t, value =
                    5 + lcast_precision<Source>::value + 6
                );
        };
    }

    namespace detail // lexical_cast_stream_traits<Source, Target>
    {
        template <class Source, class Target>
        struct lexical_cast_stream_traits {
            typedef typename boost::detail::array_to_pointer_decay<Source>::type src;
            typedef typename boost::remove_cv<src>::type            no_cv_src;

            typedef boost::detail::deduce_source_char<no_cv_src>                           deduce_src_char_metafunc;
            typedef typename deduce_src_char_metafunc::type           src_char_t;
            typedef typename boost::detail::deduce_target_char<Target>::type target_char_t;

            typedef typename boost::detail::widest_char<
                target_char_t, src_char_t
            >::type char_type;

#if !defined(BOOST_NO_CXX11_CHAR16_T) && defined(BOOST_NO_CXX11_UNICODE_LITERALS)
            static_assert(!boost::is_same<char16_t, src_char_t>::value
                                        && !boost::is_same<char16_t, target_char_t>::value,
                "Your compiler does not have full support for char16_t" );
#endif
#if !defined(BOOST_NO_CXX11_CHAR32_T) && defined(BOOST_NO_CXX11_UNICODE_LITERALS)
            static_assert(!boost::is_same<char32_t, src_char_t>::value
                                        && !boost::is_same<char32_t, target_char_t>::value,
                "Your compiler does not have full support for char32_t" );
#endif

            typedef std::char_traits<char_type> traits;

            typedef boost::detail::lcast_src_length<no_cv_src> len_t;
        };
    }

    namespace detail
    {
        template<typename Target, typename Source>
        struct lexical_converter_impl
        {
            typedef lexical_cast_stream_traits<Source, Target>  stream_trait;

            typedef detail::lcast::optimized_src_stream<
                typename stream_trait::char_type,
                typename stream_trait::traits,
                stream_trait::len_t::value + 1
            > optimized_src_stream;
            
            template <class T>
            static auto detect_type(int)
                -> decltype(std::declval<optimized_src_stream&>().stream_in(std::declval<lcast::exact<T>>()), optimized_src_stream{});

            template <class T>
            static lcast::ios_src_stream<typename stream_trait::char_type, typename stream_trait::traits> detect_type(...);

            using from_src_stream = decltype(detect_type<Source>(1));

            typedef detail::lcast::to_target_stream<
                typename stream_trait::char_type,
                typename stream_trait::traits
            > to_target_stream;

            static inline bool try_convert(const Source& arg, Target& result) {
                from_src_stream src_stream;
                if (!src_stream.stream_in(lcast::exact<Source>{arg}))
                    return false;

                to_target_stream out(src_stream.cbegin(), src_stream.cend());
                if (!out.stream_out(result))
                    return false;

                return true;
            }
        };
    }

} // namespace boost

#undef BOOST_LCAST_NO_WCHAR_T

#endif // BOOST_LEXICAL_CAST_DETAIL_CONVERTER_LEXICAL_HPP

