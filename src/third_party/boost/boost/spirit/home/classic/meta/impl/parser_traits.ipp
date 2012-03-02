/*=============================================================================
    Copyright (c) 2002-2003 Joel de Guzman
    Copyright (c) 2002-2003 Hartmut Kaiser
    Copyright (c) 2003 Martin Wille
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_PARSER_TRAITS_IPP)
#define BOOST_SPIRIT_PARSER_TRAITS_IPP

#include <boost/spirit/home/classic/core/composite/operators.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace boost { namespace spirit {

BOOST_SPIRIT_CLASSIC_NAMESPACE_BEGIN

namespace impl
{

#if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)

    ///////////////////////////////////////////////////////////////////////////
    //
    //  from spirit 1.1 (copyright (c) 2001 Bruce Florman)
    //  various workarounds to support compile time decisions without partial
    //  template specialization whether a given type is an instance of a
    //  concrete parser type.
    //
    ///////////////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////////////
    template <typename T>
    struct parser_type_traits
    {
    //  Determine at compile time (without partial specialization)
    //  whether a given type is an instance of the alternative<A,B>

        static T t();

        typedef struct { char dummy[1]; }   size1_t;
        typedef struct { char dummy[2]; }   size2_t;
        typedef struct { char dummy[3]; }   size3_t;
        typedef struct { char dummy[4]; }   size4_t;
        typedef struct { char dummy[5]; }   size5_t;
        typedef struct { char dummy[6]; }   size6_t;
        typedef struct { char dummy[7]; }   size7_t;
        typedef struct { char dummy[8]; }   size8_t;
        typedef struct { char dummy[9]; }   size9_t;
        typedef struct { char dummy[10]; }  size10_t;

    // the following functions need no implementation
        template <typename A, typename B>
        static size1_t test_(alternative<A, B> const&);
        template <typename A, typename B>
        static size2_t test_(sequence<A, B> const&);
        template <typename A, typename B>
        static size3_t test_(sequential_or<A, B> const&);
        template <typename A, typename B>
        static size4_t test_(intersection<A, B> const&);
        template <typename A, typename B>
        static size5_t test_(difference<A, B> const&);
        template <typename A, typename B>
        static size6_t test_(exclusive_or<A, B> const&);
        template <typename S>
        static size7_t test_(optional<S> const&);
        template <typename S>
        static size8_t test_(kleene_star<S> const&);
        template <typename S>
        static size9_t test_(positive<S> const&);

        static size10_t test_(...);

        BOOST_STATIC_CONSTANT(bool,
            is_alternative = (sizeof(size1_t) == sizeof(test_(t()))) );
        BOOST_STATIC_CONSTANT(bool,
            is_sequence = (sizeof(size2_t) == sizeof(test_(t()))) );
        BOOST_STATIC_CONSTANT(bool,
            is_sequential_or = (sizeof(size3_t) == sizeof(test_(t()))) );
        BOOST_STATIC_CONSTANT(bool,
            is_intersection = (sizeof(size4_t) == sizeof(test_(t()))) );
        BOOST_STATIC_CONSTANT(bool,
            is_difference = (sizeof(size5_t) == sizeof(test_(t()))) );
        BOOST_STATIC_CONSTANT(bool,
            is_exclusive_or = (sizeof(size6_t) == sizeof(test_(t()))) );
        BOOST_STATIC_CONSTANT(bool,
            is_optional = (sizeof(size7_t) == sizeof(test_(t()))) );
        BOOST_STATIC_CONSTANT(bool,
            is_kleene_star = (sizeof(size8_t) == sizeof(test_(t()))) );
        BOOST_STATIC_CONSTANT(bool,
            is_positive = (sizeof(size9_t) == sizeof(test_(t()))) );
    };

#else

    ///////////////////////////////////////////////////////////////////////////
    struct parser_type_traits_base {

        BOOST_STATIC_CONSTANT(bool, is_alternative = false);
        BOOST_STATIC_CONSTANT(bool, is_sequence = false);
        BOOST_STATIC_CONSTANT(bool, is_sequential_or = false);
        BOOST_STATIC_CONSTANT(bool, is_intersection = false);
        BOOST_STATIC_CONSTANT(bool, is_difference = false);
        BOOST_STATIC_CONSTANT(bool, is_exclusive_or = false);
        BOOST_STATIC_CONSTANT(bool, is_optional = false);
        BOOST_STATIC_CONSTANT(bool, is_kleene_star = false);
        BOOST_STATIC_CONSTANT(bool, is_positive = false);
    };

    template <typename ParserT>
    struct parser_type_traits : public parser_type_traits_base {

    //  no definition here, fallback for all not explicitly mentioned parser
    //  types
    };

    template <typename A, typename B>
    struct parser_type_traits<alternative<A, B> >
    :   public parser_type_traits_base {

        BOOST_STATIC_CONSTANT(bool, is_alternative = true);
    };

    template <typename A, typename B>
    struct parser_type_traits<sequence<A, B> >
    :   public parser_type_traits_base {

        BOOST_STATIC_CONSTANT(bool, is_sequence = true);
    };

    template <typename A, typename B>
    struct parser_type_traits<sequential_or<A, B> >
    :   public parser_type_traits_base {

        BOOST_STATIC_CONSTANT(bool, is_sequential_or = true);
    };

    template <typename A, typename B>
    struct parser_type_traits<intersection<A, B> >
    :   public parser_type_traits_base {

        BOOST_STATIC_CONSTANT(bool, is_intersection = true);
    };

    template <typename A, typename B>
    struct parser_type_traits<difference<A, B> >
    :   public parser_type_traits_base {

        BOOST_STATIC_CONSTANT(bool, is_difference = true);
    };

    template <typename A, typename B>
    struct parser_type_traits<exclusive_or<A, B> >
    :   public parser_type_traits_base {

        BOOST_STATIC_CONSTANT(bool, is_exclusive_or = true);
    };

    template <typename S>
    struct parser_type_traits<optional<S> >
    :   public parser_type_traits_base {

        BOOST_STATIC_CONSTANT(bool, is_optional = true);
    };

    template <typename S>
    struct parser_type_traits<kleene_star<S> >
    :   public parser_type_traits_base {

        BOOST_STATIC_CONSTANT(bool, is_kleene_star = true);
    };

    template <typename S>
    struct parser_type_traits<positive<S> >
    :   public parser_type_traits_base {

        BOOST_STATIC_CONSTANT(bool, is_positive = true);
    };

#endif // defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
}   // namespace impl

///////////////////////////////////////////////////////////////////////////////
BOOST_SPIRIT_CLASSIC_NAMESPACE_END

}} // namespace boost::spirit

#endif // !defined(BOOST_SPIRIT_PARSER_TRAITS_IPP)
