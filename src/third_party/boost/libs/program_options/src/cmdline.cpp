// Copyright Vladimir Prus 2002-2004.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)

#define BOOST_PROGRAM_OPTIONS_SOURCE
#include <boost/program_options/config.hpp>

#include <boost/config.hpp>

#include <boost/program_options/detail/cmdline.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/positional_options.hpp>
#include <boost/throw_exception.hpp>

#include <boost/bind.hpp>

#include <string>
#include <utility>
#include <vector>
#include <cassert>
#include <cstring>
#include <cctype>
#include <climits>

#include <cstdio>

#include <iostream>

namespace boost { namespace program_options {

    using namespace std;
    using namespace boost::program_options::command_line_style;
    
    invalid_syntax::
    invalid_syntax(const string& tokens, kind_t kind)
     : error(error_message(kind).append(" in '").append(tokens).append("'"))
     , m_tokens(tokens)
     , m_kind(kind)                       
    {}
    
    string 
    invalid_syntax::error_message(kind_t kind)
    {
        // Initially, store the message in 'const char*' variable,
        // to avoid conversion to string in all cases.
        const char* msg;
        switch(kind)
        {
        case long_not_allowed:
            msg = "long options are not allowed";
            break;
        case long_adjacent_not_allowed:
            msg = "parameters adjacent to long options not allowed";
            break;
        case short_adjacent_not_allowed:
            msg = "parameters adjust to short options are not allowed";
            break;
        case empty_adjacent_parameter:
            msg = "adjacent parameter is empty";
            break;
        case missing_parameter:
            msg = "required parameter is missing";
            break;
        case extra_parameter:
            msg = "extra parameter";
            break;
        case unrecognized_line:
            msg = "unrecognized line";
            break;
        default:
            msg = "unknown error";
        }
        return msg;
    }

    invalid_syntax::kind_t 
    invalid_syntax::kind() const
    {
        return m_kind;
    }
    
    const string& 
    invalid_syntax::tokens() const
    {
        return m_tokens;
    }

    invalid_command_line_syntax::
    invalid_command_line_syntax(const string& tokens, kind_t kind)
    : invalid_syntax(tokens, kind)
    {}

}}


namespace boost { namespace program_options { namespace detail {

    // vc6 needs this, but borland chokes when this is added.
#if BOOST_WORKAROUND(_MSC_VER, < 1300)
    using namespace std;
    using namespace program_options;
#endif


    cmdline::cmdline(const vector<string>& args)
    {
        init(args);
    }

    cmdline::cmdline(int argc, const char*const * argv)
    {
#if defined(BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS)
        vector<string> args;
        copy(argv+1, argv+argc+!argc, inserter(args, args.end()));
        init(args);
#else
        init(vector<string>(argv+1, argv+argc+!argc));
#endif
    }

    void
    cmdline::init(const vector<string>& args)
    {
        this->args = args;        
        m_style = command_line_style::default_style;
        m_desc = 0;
        m_positional = 0;
        m_allow_unregistered = false;
    }

    void 
    cmdline::style(int style)
    {
        if (style == 0) 
            style = default_style;        

        check_style(style);
        this->m_style = style_t(style);
    }
    
    void 
    cmdline::allow_unregistered()
    {
        this->m_allow_unregistered = true;
    }

    void 
    cmdline::check_style(int style) const
    {
        bool allow_some_long = 
            (style & allow_long) || (style & allow_long_disguise);

        const char* error = 0;
        if (allow_some_long && 
            !(style & long_allow_adjacent) && !(style & long_allow_next))
            error = "style disallows parameters for long options";

        if (!error && (style & allow_short) &&
            !(style & short_allow_adjacent) && !(style & short_allow_next))
            error = "style disallows parameters for short options";

        if (!error && (style & allow_short) &&
            !(style & allow_dash_for_short) && !(style & allow_slash_for_short))
            error = "style disallows all characters for short options";

        if (error)
            boost::throw_exception(invalid_command_line_style(error));

        // Need to check that if guessing and long disguise are enabled
        // -f will mean the same as -foo
    }
    
    bool 
    cmdline::is_style_active(style_t style) const
    {
        return ((m_style & style) ? true : false);
    }    

    void 
    cmdline::set_options_description(const options_description& desc)
    {
        m_desc = &desc;
    }

    void 
    cmdline::set_positional_options(
        const positional_options_description& positional)
    {
        m_positional = &positional;
    }


    vector<option>
    cmdline::run()
    {
        // The parsing is done by having a set of 'style parsers'
        // and trying then in order. Each parser is passed a vector
        // of unparsed tokens and can consume some of them (by
        // removing elements on front) and return a vector of options.
        //
        // We try each style parser in turn, untill some input
        // is consumed. The returned vector of option may contain the
        // result of just syntactic parsing of token, say --foo will
        // be parsed as option with name 'foo', and the style parser
        // is not required to care if that option is defined, and how
        // many tokens the value may take.
        // So, after vector is returned, we validate them.
        assert(m_desc);

        vector<style_parser> style_parsers;      

        if (m_style_parser)
            style_parsers.push_back(m_style_parser);

        if (m_additional_parser)
            style_parsers.push_back(
                boost::bind(&cmdline::handle_additional_parser, this, _1));

        if (m_style & allow_long)
            style_parsers.push_back(
                boost::bind(&cmdline::parse_long_option, this, _1));

        if ((m_style & allow_long_disguise))
            style_parsers.push_back(
                boost::bind(&cmdline::parse_disguised_long_option, this, _1));

        if ((m_style & allow_short) && (m_style & allow_dash_for_short))
            style_parsers.push_back(
                boost::bind(&cmdline::parse_short_option, this, _1));

        if ((m_style & allow_short) && (m_style & allow_slash_for_short))
            style_parsers.push_back(boost::bind(&cmdline::parse_dos_option, this, _1));

        style_parsers.push_back(boost::bind(&cmdline::parse_terminator, this, _1));

        vector<option> result;
        while(!args.empty())
        {
            bool ok = false;
            for(unsigned i = 0; i < style_parsers.size(); ++i)
            {
                unsigned current_size = args.size();
                vector<option> next = style_parsers[i](args);

                // Check that option names
                // are valid, and that all values are in place.
                if (!next.empty())
                {
                    vector<string> e;
                    for(unsigned k = 0; k < next.size()-1; ++k) {
                        finish_option(next[k], e, style_parsers);
                    }
                    // For the last option, pass the unparsed tokens
                    // so that they can be added to next.back()'s values
                    // if appropriate.
                    finish_option(next.back(), args, style_parsers);
                    for (unsigned j = 0; j < next.size(); ++j)
                        result.push_back(next[j]);                    
                }
                                
                if (args.size() != current_size) {
                    ok = true;
                    break;                
                } 
            }
            
            if (!ok) {
                option opt;
                opt.value.push_back(args[0]);
                opt.original_tokens.push_back(args[0]);
                result.push_back(opt);
                args.erase(args.begin());
            }
        }

        /* If an key option is followed by a positional option,
           can can consume more tokens (e.g. it's multitoke option),
           give those tokens to it.  */
        vector<option> result2;
        for (unsigned i = 0; i < result.size(); ++i)
        {
            result2.push_back(result[i]);
            option& opt = result2.back();

            if (opt.string_key.empty())
                continue;

            const option_description* xd = 
                m_desc->find_nothrow(opt.string_key, 
                                        is_style_active(allow_guessing),
                                        is_style_active(long_case_insensitive),
                                        is_style_active(short_case_insensitive));
            if (!xd)
                continue;

            unsigned min_tokens = xd->semantic()->min_tokens();
            unsigned max_tokens = xd->semantic()->max_tokens();
            if (min_tokens < max_tokens && opt.value.size() < max_tokens)
            {
                // This option may grab some more tokens.
                // We only allow to grab tokens that are not already
                // recognized as key options.

                int can_take_more = max_tokens - opt.value.size();
                unsigned j = i+1;
                for (; can_take_more && j < result.size(); --can_take_more, ++j)
                {
                    option& opt2 = result[j];
                    if (!opt2.string_key.empty())
                        break;

                    if (opt2.position_key == INT_MAX)
                    {
                        // We use INT_MAX to mark positional options that
                        // were found after the '--' terminator and therefore
                        // should stay positional forever.
                        break;
                    }

                    assert(opt2.value.size() == 1);
                    
                    opt.value.push_back(opt2.value[0]);

                    assert(opt2.original_tokens.size() == 1);

                    opt.original_tokens.push_back(opt2.original_tokens[0]);
                }
                i = j-1;
            }
        }
        result.swap(result2);
        

        // Assign position keys to positional options.
        int position_key = 0;
        for(unsigned i = 0; i < result.size(); ++i) {
            if (result[i].string_key.empty())
                result[i].position_key = position_key++;
        }

        if (m_positional)
        {
            unsigned position = 0;
            for (unsigned i = 0; i < result.size(); ++i) {
                option& opt = result[i];
                if (opt.position_key != -1) {
                    if (position >= m_positional->max_total_count())
                    {
                        boost::throw_exception(too_many_positional_options_error());
                    }
                    opt.string_key = m_positional->name_for_position(position);
                    ++position;
                }
            }
        }
        
        // set case sensitive flag
        for (unsigned i = 0; i < result.size(); ++i) {
            if (result[i].string_key.size() > 2 ||
                        (result[i].string_key.size() > 1 && result[i].string_key[0] != '-'))
            {
                // it is a long option
                result[i].case_insensitive = is_style_active(long_case_insensitive);
            }
            else
            {
                // it is a short option
                result[i].case_insensitive = is_style_active(short_case_insensitive);
            }
        }

        return result;
    }

    void
    cmdline::finish_option(option& opt,
                           vector<string>& other_tokens,
                           const vector<style_parser>& style_parsers)
    {          
        if (opt.string_key.empty())
            return;

        // First check that the option is valid, and get its description.
        const option_description* xd = m_desc->find_nothrow(opt.string_key, 
                is_style_active(allow_guessing),
                is_style_active(long_case_insensitive),
                is_style_active(short_case_insensitive));

        if (!xd)
        {
            if (m_allow_unregistered) {
                opt.unregistered = true;
                return;
            } else {
                boost::throw_exception(unknown_option(opt.string_key));
            }                
        }
        const option_description& d = *xd;

        // Canonize the name
        opt.string_key = d.key(opt.string_key);

        // We check that the min/max number of tokens for the option
        // agrees with the number of tokens we have. The 'adjacent_value'
        // (the value in --foo=1) counts as a separate token, and if present
        // must be consumed. The following tokens on the command line may be
        // left unconsumed.

        // We don't check if those tokens look like option, or not!

        unsigned min_tokens = d.semantic()->min_tokens();
        unsigned max_tokens = d.semantic()->max_tokens();
        
        unsigned present_tokens = opt.value.size() + other_tokens.size();
        
        if (present_tokens >= min_tokens)
        {
            if (!opt.value.empty() && max_tokens == 0) 
            {
                boost::throw_exception(invalid_command_line_syntax(opt.string_key,
                                             invalid_command_line_syntax::extra_parameter));
            }
            
            // If an option wants, at minimum, N tokens, we grab them there,
            // when adding these tokens as values to current option we check
            // if they look like options
            if (opt.value.size() <= min_tokens) 
            {
                min_tokens -= opt.value.size();
            }
            else
            {
                min_tokens = 0;
            }

            // Everything's OK, move the values to the result.            
            for(;!other_tokens.empty() && min_tokens--; ) 
            {
                // check if extra parameter looks like a known option
                // we use style parsers to check if it is syntactically an option, 
                // additionally we check if an option_description exists
                vector<option> followed_option;  
                vector<string> next_token(1, other_tokens[0]);      
                for (unsigned i = 0; followed_option.empty() && i < style_parsers.size(); ++i)
                {
                    followed_option = style_parsers[i](next_token);
                }
                if (!followed_option.empty()) 
                {
                    const option_description* od = m_desc->find_nothrow(other_tokens[0], 
                              is_style_active(allow_guessing),
                              is_style_active(long_case_insensitive),
                              is_style_active(short_case_insensitive));
                    if (od) 
                        boost::throw_exception(invalid_command_line_syntax(opt.string_key,
                                                    invalid_command_line_syntax::missing_parameter));
                }
                opt.value.push_back(other_tokens[0]);
                opt.original_tokens.push_back(other_tokens[0]);
                other_tokens.erase(other_tokens.begin());
            }
        }
        else
        {
            boost::throw_exception(invalid_command_line_syntax(opt.string_key,
                                            invalid_command_line_syntax::missing_parameter)); 

        }
    }

    vector<option> 
    cmdline::parse_long_option(vector<string>& args)
    {
        vector<option> result;
        const string& tok = args[0];
        if (tok.size() >= 3 && tok[0] == '-' && tok[1] == '-')
        {   
            string name, adjacent;

            string::size_type p = tok.find('=');
            if (p != tok.npos)
            {
                name = tok.substr(2, p-2);
                adjacent = tok.substr(p+1);
                if (adjacent.empty())
                    boost::throw_exception( invalid_command_line_syntax(name,
                                                      invalid_command_line_syntax::empty_adjacent_parameter) );
            }
            else
            {
                name = tok.substr(2);
            }
            option opt;
            opt.string_key = name;
            if (!adjacent.empty())
                opt.value.push_back(adjacent);
            opt.original_tokens.push_back(tok);
            result.push_back(opt);
            args.erase(args.begin());
        }
        return result;
    }


    vector<option> 
    cmdline::parse_short_option(vector<string>& args)
    {
        const string& tok = args[0];
        if (tok.size() >= 2 && tok[0] == '-' && tok[1] != '-')
        {   
            vector<option> result;

            string name = tok.substr(0,2);
            string adjacent = tok.substr(2);

            // Short options can be 'grouped', so that
            // "-d -a" becomes "-da". Loop, processing one
            // option at a time. We exit the loop when either
            // we've processed all the token, or when the remainder
            // of token is considered to be value, not further grouped
            // option.
            for(;;) {
                const option_description* d 
                    = m_desc->find_nothrow(name, false, false,
                            is_style_active(short_case_insensitive));

                // FIXME: check for 'allow_sticky'.
                if (d && (m_style & allow_sticky) &&
                    d->semantic()->max_tokens() == 0 && !adjacent.empty()) {
                    // 'adjacent' is in fact further option.
                    option opt;
                    opt.string_key = name;
                    result.push_back(opt);

                    if (adjacent.empty())
                    {
                        args.erase(args.begin());
                        break;
                    }

                    name = string("-") + adjacent[0];
                    adjacent.erase(adjacent.begin());
                } else {
                    
                    option opt;
                    opt.string_key = name;
                    opt.original_tokens.push_back(tok);
                    if (!adjacent.empty())
                        opt.value.push_back(adjacent);
                    result.push_back(opt);
                    args.erase(args.begin());                    
                    break;
                }
            }
            return result;
        }
        return vector<option>();
    }

    vector<option> 
    cmdline::parse_dos_option(vector<string>& args)
    {
        vector<option> result;
        const string& tok = args[0];
        if (tok.size() >= 2 && tok[0] == '/')
        {   
            string name = "-" + tok.substr(1,1);
            string adjacent = tok.substr(2);

            option opt;
            opt.string_key = name;
            if (!adjacent.empty())
                opt.value.push_back(adjacent);
            opt.original_tokens.push_back(tok);
            result.push_back(opt);
            args.erase(args.begin());
        }
        return result;
    }

    vector<option> 
    cmdline::parse_disguised_long_option(vector<string>& args)
    {
        const string& tok = args[0];
        if (tok.size() >= 2 && 
            ((tok[0] == '-' && tok[1] != '-') ||
             ((m_style & allow_slash_for_short) && tok[0] == '/')))            
        {
            if (m_desc->find_nothrow(tok.substr(1, tok.find('=')-1), 
                                     is_style_active(allow_guessing),
                                     is_style_active(long_case_insensitive),
                                     is_style_active(short_case_insensitive)))
            {
                args[0].insert(0, "-");
                if (args[0][1] == '/')
                    args[0][1] = '-';
                return parse_long_option(args);
            }
        }
        return vector<option>();
    }

    vector<option> 
    cmdline::parse_terminator(vector<string>& args)
    {
        vector<option> result;
        const string& tok = args[0];
        if (tok == "--")
        {
            for(unsigned i = 1; i < args.size(); ++i)
            {
                option opt;
                opt.value.push_back(args[i]);
                opt.original_tokens.push_back(args[i]);
                opt.position_key = INT_MAX;
                result.push_back(opt);
            }
            args.clear();
        }
        return result;
    }

    vector<option> 
    cmdline::handle_additional_parser(vector<string>& args)
    {
        vector<option> result;
        pair<string, string> r = m_additional_parser(args[0]);
        if (!r.first.empty()) {
            option next;
            next.string_key = r.first;
            if (!r.second.empty())
                next.value.push_back(r.second);
            result.push_back(next);
            args.erase(args.begin());
        }
        return result;
    }

    void 
    cmdline::set_additional_parser(additional_parser p)
    {
        m_additional_parser = p;
    }

    void 
    cmdline::extra_style_parser(style_parser s)
    {
        m_style_parser = s;
    }



}}}
