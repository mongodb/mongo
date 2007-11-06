/*=============================================================================
    Copyright (C) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_NUMERICS_FWD_HPP)
#   define BOOST_SPIRIT_NUMERICS_FWD_HPP

namespace boost { namespace spirit 
{
    ///////////////////////////////////////////////////////////////////////////
    //
    //  uint_parser class
    //
    ///////////////////////////////////////////////////////////////////////////
    template <
        typename T = unsigned,
        int Radix = 10,
        unsigned MinDigits = 1,
        int MaxDigits = -1
    >
    struct uint_parser;

    ///////////////////////////////////////////////////////////////////////////
    //
    //  int_parser class
    //
    ///////////////////////////////////////////////////////////////////////////
    template <
        typename T = unsigned,
        int Radix = 10,
        unsigned MinDigits = 1,
        int MaxDigits = -1
    >
    struct int_parser;

    ///////////////////////////////////////////////////////////////////////////
    //
    //  sign_parser class
    //
    ///////////////////////////////////////////////////////////////////////////
    struct sign_parser;

    ///////////////////////////////////////////////////////////////////////////
    //
    //  default real number policies
    //
    ///////////////////////////////////////////////////////////////////////////
    template <typename T>
    struct ureal_parser_policies;

    template <typename T>
    struct real_parser_policies;

    ///////////////////////////////////////////////////////////////////////////
    //
    //  real_parser class
    //
    ///////////////////////////////////////////////////////////////////////////
    template <
        typename T = double,
        typename RealPoliciesT = ureal_parser_policies<T>
    >
    struct real_parser;

    ///////////////////////////////////////////////////////////////////////////
    //
    //  strict reals (do not allow plain integers (no decimal point))
    //
    ///////////////////////////////////////////////////////////////////////////
    template <typename T>
    struct strict_ureal_parser_policies;

    template <typename T>
    struct strict_real_parser_policies;

}} // namespace boost::spirit

#endif

