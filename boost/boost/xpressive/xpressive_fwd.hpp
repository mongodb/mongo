///////////////////////////////////////////////////////////////////////////////
/// \file xpressive_fwd.hpp
/// Forward declarations for all of xpressive's public data types.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_XPRESSIVE_FWD_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_XPRESSIVE_FWD_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <string>
#include <boost/config.hpp>
#include <boost/iterator/iterator_traits.hpp>

#ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
# error Sorry, xpressive requires a compiler that supports partial template specialization.
#endif

#if defined(BOOST_NO_STD_LOCALE) & !defined(BOOST_XPRESSIVE_USE_C_TRAITS)
# define BOOST_XPRESSIVE_USE_C_TRAITS
#endif

#if defined(BOOST_NO_CWCHAR) | defined(BOOST_NO_CWCTYPE) | defined(BOOST_NO_STD_WSTRING)
# ifndef BOOST_XPRESSIVE_NO_WREGEX
#  define BOOST_XPRESSIVE_NO_WREGEX
# endif
#endif

#include <boost/xpressive/proto/proto_fwd.hpp>

namespace boost { namespace xpressive
{
    template<typename Char, typename Impl>
    struct regex_traits;

    template<typename Char>
    struct cpp_regex_traits;

    template<typename Char>
    struct c_regex_traits;

    template<typename Elem>
    struct null_regex_traits;

    namespace detail
    {
        template<typename Char>
        struct default_regex_traits
        {
            #ifdef BOOST_XPRESSIVE_USE_C_TRAITS
            typedef c_regex_traits<Char> type;
            #else
            typedef cpp_regex_traits<Char> type;
            #endif
        };

        struct mark_placeholder;

        typedef proto::unary_op<detail::mark_placeholder, proto::noop_tag> mark_tag;

    } // namespace detail

    using detail::mark_tag;

    typedef void const *regex_id_type;

    struct regex_error;

    struct regex_traits_version_1_tag;

    struct regex_traits_version_1_case_fold_tag;

    template<typename BidiIter>
    struct basic_regex;

    template<typename BidiIter>
    struct match_results;

    template<typename BidiIter>
    struct regex_iterator;

    template<typename BidiIter>
    struct regex_token_iterator;

    template<typename BidiIter>
    struct regex_id_filter_predicate;

    template<typename BidiIter>
    struct sub_match;

    template<typename Action, typename Saved = Action>
    struct action;

    template<typename RegexTraits>
    struct compiler_traits;

    template<typename Char, typename Impl = typename detail::default_regex_traits<Char>::type>
    struct regex_traits;

    template
    <
        typename BidiIter
      , typename RegexTraits = regex_traits<typename iterator_value<BidiIter>::type>
      , typename CompilerTraits = compiler_traits<RegexTraits>
    >
    struct regex_compiler;

    ///////////////////////////////////////////////////////////////////////////////
    // Common typedefs
    //
    typedef basic_regex<std::string::const_iterator>                sregex;
    typedef basic_regex<char const *>                               cregex;

    #ifndef BOOST_XPRESSIVE_NO_WREGEX
    typedef basic_regex<std::wstring::const_iterator>               wsregex;
    typedef basic_regex<wchar_t const *>                            wcregex;
    #endif

    typedef regex_compiler<std::string::const_iterator>             sregex_compiler;
    typedef regex_compiler<char const *>                            cregex_compiler;

    #ifndef BOOST_XPRESSIVE_NO_WREGEX
    typedef regex_compiler<std::wstring::const_iterator>            wsregex_compiler;
    typedef regex_compiler<wchar_t const *>                         wcregex_compiler;
    #endif

    typedef regex_iterator<std::string::const_iterator>             sregex_iterator;
    typedef regex_iterator<char const *>                            cregex_iterator;

    #ifndef BOOST_XPRESSIVE_NO_WREGEX
    typedef regex_iterator<std::wstring::const_iterator>            wsregex_iterator;
    typedef regex_iterator<wchar_t const *>                         wcregex_iterator;
    #endif

    typedef regex_token_iterator<std::string::const_iterator>       sregex_token_iterator;
    typedef regex_token_iterator<char const *>                      cregex_token_iterator;

    #ifndef BOOST_XPRESSIVE_NO_WREGEX
    typedef regex_token_iterator<std::wstring::const_iterator>      wsregex_token_iterator;
    typedef regex_token_iterator<wchar_t const *>                   wcregex_token_iterator;
    #endif

    typedef match_results<std::string::const_iterator>              smatch;
    typedef match_results<char const *>                             cmatch;

    #ifndef BOOST_XPRESSIVE_NO_WREGEX
    typedef match_results<std::wstring::const_iterator>             wsmatch;
    typedef match_results<wchar_t const *>                          wcmatch;
    #endif

    typedef regex_id_filter_predicate<std::string::const_iterator>  sregex_id_filter_predicate;
    typedef regex_id_filter_predicate<char const *>                 cregex_id_filter_predicate;

    #ifndef BOOST_XPRESSIVE_NO_WREGEX
    typedef regex_id_filter_predicate<std::wstring::const_iterator> wsregex_id_filter_predicate;
    typedef regex_id_filter_predicate<wchar_t const *>              wcregex_id_filter_predicate;
    #endif

}} // namespace boost::xpressive

#endif
