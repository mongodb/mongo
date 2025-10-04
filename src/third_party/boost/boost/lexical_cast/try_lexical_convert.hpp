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

#ifndef BOOST_LEXICAL_CAST_TRY_LEXICAL_CONVERT_HPP
#define BOOST_LEXICAL_CAST_TRY_LEXICAL_CONVERT_HPP

#include <boost/config.hpp>
#ifdef BOOST_HAS_PRAGMA_ONCE
#   pragma once
#endif

#include <boost/type_traits/conditional.hpp>
#include <boost/type_traits/is_arithmetic.hpp>

#include <boost/lexical_cast/detail/buffer_view.hpp>
#include <boost/lexical_cast/detail/is_character.hpp>
#include <boost/lexical_cast/detail/converter_numeric.hpp>
#include <boost/lexical_cast/detail/converter_lexical.hpp>

namespace boost {
    namespace detail
    {
        template<typename Target, typename Source>
        using is_arithmetic_and_not_xchars = boost::integral_constant<
            bool,
            !(boost::detail::is_character<Target>::value) &&
                !(boost::detail::is_character<Source>::value) &&
                boost::is_arithmetic<Source>::value &&
                boost::is_arithmetic<Target>::value
        >;
    }

    namespace conversion { namespace detail {

        template <typename Target, typename Source>
        inline bool try_lexical_convert(const Source& arg, Target& result)
        {
            static_assert(
                !boost::is_volatile<Source>::value,
                "Boost.LexicalCast does not support volatile input");

            typedef typename boost::detail::array_to_pointer_decay<Source>::type src;

            typedef boost::detail::is_arithmetic_and_not_xchars<Target, src >
                shall_we_copy_with_dynamic_check_t;

            typedef typename boost::conditional<
                 shall_we_copy_with_dynamic_check_t::value,
                 boost::detail::dynamic_num_converter_impl<Target, src >,
                 boost::detail::lexical_converter_impl<Target, src >
            >::type caster_type;

            return caster_type::try_convert(arg, result);
        }

        template <typename Target, typename CharacterT>
        inline bool try_lexical_convert(const CharacterT* chars, std::size_t count, Target& result)
        {
            static_assert(
                boost::detail::is_character<CharacterT>::value,
                "This overload of try_lexical_convert is meant to be used only with arrays of characters."
            );
            return ::boost::conversion::detail::try_lexical_convert(
                ::boost::conversion::detail::make_buffer_view(chars, chars + count),
                result
            );
        }

    }} // namespace conversion::detail

    namespace conversion {
        // ADL barrier
        using ::boost::conversion::detail::try_lexical_convert;
    }

} // namespace boost

#endif // BOOST_LEXICAL_CAST_TRY_LEXICAL_CONVERT_HPP

