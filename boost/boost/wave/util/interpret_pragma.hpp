/*=============================================================================
    Boost.Wave: A Standard compliant C++ preprocessor library

    http://www.boost.org/

    Copyright (c) 2001-2007 Hartmut Kaiser. Distributed under the Boost
    Software License, Version 1.0. (See accompanying file
    LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#if !defined(INTERPRET_PRAGMA_HPP_B1F2315E_C5CE_4ED1_A343_0EF548B7942A_INCLUDED)
#define INTERPRET_PRAGMA_HPP_B1F2315E_C5CE_4ED1_A343_0EF548B7942A_INCLUDED

#include <string>
#include <list>

#include <boost/spirit/core.hpp>
#if SPIRIT_VERSION >= 0x1700
#include <boost/spirit/actor/assign_actor.hpp>
#include <boost/spirit/actor/push_back_actor.hpp>
#endif // SPIRIT_VERSION >= 0x1700

#include <boost/wave/wave_config.hpp>

#include <boost/wave/util/pattern_parser.hpp>
#include <boost/wave/util/macro_helpers.hpp>

#include <boost/wave/token_ids.hpp>
#include <boost/wave/cpp_exceptions.hpp>
#include <boost/wave/cpp_iteration_context.hpp>
#include <boost/wave/language_support.hpp>

#if !defined(spirit_append_actor)
#if SPIRIT_VERSION >= 0x1700
#define spirit_append_actor(actor) boost::spirit::push_back_a(actor)
#define spirit_assign_actor(actor) boost::spirit::assign_a(actor)
#else
#define spirit_append_actor(actor) boost::spirit::append(actor)
#define spirit_assign_actor(actor) boost::spirit::assign(actor)
#endif // SPIRIT_VERSION >= 0x1700
#endif // !defined(spirit_append_actor)

// this must occur after all of the includes and before any code appears
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_PREFIX
#endif

///////////////////////////////////////////////////////////////////////////////
namespace boost {
namespace wave {
namespace util {

///////////////////////////////////////////////////////////////////////////////
//
//  The function interpret_pragma interprets the given token sequence as the
//  body of a #pragma directive (or parameter to the _Pragma operator) and 
//  executes the actions associated with recognized Wave specific options.
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT, typename IteratorT, typename ContainerT>
inline bool 
interpret_pragma(ContextT &ctx, typename ContextT::token_type const &act_token,
    IteratorT it, IteratorT const &end, ContainerT &pending)
{
    typedef typename ContextT::token_type token_type;
    typedef typename token_type::string_type string_type;
    
    using namespace cpplexer;
    if (T_IDENTIFIER == token_id(*it)) {
    // check for pragma wave ...
        if ((*it).get_value() == BOOST_WAVE_PRAGMA_KEYWORD) 
        {
        //  this is a wave specific option, it should have the form:
        //
        //      #pragma command option(value)
        //
        //  where 
        //      'command' is the value of the preprocessor constant
        //                BOOST_WAVE_PRAGMA_KEYWORD (defaults to "wave") and
        //      '(value)' is required only for some pragma directives (this is 
        //                optional)
        //
        //  All recognized #pragma operators are forwarded to the supplied 
        //  preprocessing hook.
            using namespace boost::spirit;
            token_type option;
            ContainerT values;
            
            if (!parse (++it, end, 
                            (   ch_p(T_IDENTIFIER)
                                [
                                    spirit_assign_actor(option)
                                ] 
                            |   pattern_p(KeywordTokenType, TokenTypeMask)
                                [
                                    spirit_assign_actor(option)
                                ] 
                            |   pattern_p(OperatorTokenType|AltExtTokenType, 
                                    ExtTokenTypeMask)   // and, bit_and etc.
                                [
                                    spirit_assign_actor(option)
                                ] 
                            |   pattern_p(BoolLiteralTokenType, TokenTypeMask)
                                [
                                    spirit_assign_actor(option)
                                ] 
                            )
                        >> !(   ch_p(T_LEFTPAREN) 
                            >>  lexeme_d[
                                    *(anychar_p[spirit_append_actor(values)] - ch_p(T_RIGHTPAREN))
                                ]
                            >>  ch_p(T_RIGHTPAREN)
                            ),
                    pattern_p(WhiteSpaceTokenType, TokenTypeMask)).hit)
            {
                BOOST_WAVE_THROW(preprocess_exception, ill_formed_pragma_option,
                    impl::as_string<string_type>(it, end).c_str(), 
                    act_token.get_position());
            }
        
        // remove the falsely matched closing parenthesis
            if (values.size() > 0) {
                BOOST_ASSERT(T_RIGHTPAREN == values.back());
                typename ContainerT::reverse_iterator rit = values.rbegin();
                values.erase((++rit).base());
            }
            
        // decode the option (call the context_policy hook)
            if (!ctx.interpret_pragma(pending, option, values, act_token)) {
            // unknown #pragma option 
            string_type option_str ((*it).get_value());

                option_str += option.get_value();
                if (values.size() > 0) {
                    option_str += "(";
                    option_str += impl::as_string(values);
                    option_str += ")";
                }
                BOOST_WAVE_THROW(preprocess_exception, ill_formed_pragma_option,
                    option_str.c_str(), act_token.get_position());
            }
            return true;
        }
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
        else if ((*it).get_value() == "once") {
        // #pragma once
            return ctx.add_pragma_once_header(ctx.get_current_filename());
        }
#endif 
#if BOOST_WAVE_SUPPORT_PRAGMA_MESSAGE != 0
        else if ((*it).get_value() == "message") {
        // #pragma message(...) or #pragma message ...
            using namespace boost::spirit;
            ContainerT values;
            
            if (!parse (++it, end, 
                            (   (   ch_p(T_LEFTPAREN) 
                                >>  lexeme_d[
                                        *(anychar_p[spirit_append_actor(values)] - ch_p(T_RIGHTPAREN))
                                    ]
                                >>  ch_p(T_RIGHTPAREN)
                                )
                            |   lexeme_d[
                                    *(anychar_p[spirit_append_actor(values)] - ch_p(T_NEWLINE))
                                ]
                            ),
                            pattern_p(WhiteSpaceTokenType, TokenTypeMask)
                       ).hit
               )
            {
                BOOST_WAVE_THROW(preprocess_exception, ill_formed_pragma_message,
                    impl::as_string<string_type>(it, end).c_str(), 
                    act_token.get_position());
            }
        
        // remove the falsely matched closing parenthesis/newline
            if (values.size() > 0) {
                BOOST_ASSERT(T_RIGHTPAREN == values.back() || T_NEWLINE == values.back());
                typename ContainerT::reverse_iterator rit = values.rbegin();
                values.erase((++rit).base());
            }

        // output the message itself
            BOOST_WAVE_THROW(preprocess_exception, pragma_message_directive, 
                impl::as_string(values).c_str(), act_token.get_position());
        }
#endif
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////
}   // namespace util
}   // namespace wave
}   // namespace boost

// the suffix header occurs after all of the code
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_SUFFIX
#endif

#endif // !defined(INTERPRET_PRAGMA_HPP_B1F2315E_C5CE_4ED1_A343_0EF548B7942A_INCLUDED)
