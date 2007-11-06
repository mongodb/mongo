/*=============================================================================
    Boost.Wave: A Standard compliant C++ preprocessor library

    Definition of the preprocessor iterator
    
    http://www.boost.org/

    Copyright (c) 2001-2007 Hartmut Kaiser. Distributed under the Boost
    Software License, Version 1.0. (See accompanying file
    LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#if !defined(CPP_ITERATOR_HPP_175CA88F_7273_43FA_9039_BCF7459E1F29_INCLUDED)
#define CPP_ITERATOR_HPP_175CA88F_7273_43FA_9039_BCF7459E1F29_INCLUDED

#include <string>
#include <vector>
#include <list>
#include <cstdlib>
#include <cctype>

#include <boost/assert.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/spirit/iterator/multi_pass.hpp>
#include <boost/spirit/tree/parse_tree_utils.hpp>

#include <boost/wave/wave_config.hpp>
#include <boost/pool/pool_alloc.hpp>

#include <boost/wave/util/insert_whitespace_detection.hpp>
#include <boost/wave/util/macro_helpers.hpp>
#include <boost/wave/util/cpp_macromap_utils.hpp>
#include <boost/wave/util/interpret_pragma.hpp>
#include <boost/wave/util/transform_iterator.hpp>
#include <boost/wave/util/functor_input.hpp>

#include <boost/wave/grammars/cpp_grammar_gen.hpp>
#include <boost/wave/grammars/cpp_expression_grammar_gen.hpp>
#if BOOST_WAVE_ENABLE_COMMANDLINE_MACROS != 0
#include <boost/wave/grammars/cpp_predef_macros_gen.hpp>
#endif

#include <boost/wave/whitespace_handling.hpp>
#include <boost/wave/cpp_iteration_context.hpp>
#include <boost/wave/cpp_exceptions.hpp>
#include <boost/wave/language_support.hpp>

// this must occur after all of the includes and before any code appears
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_PREFIX
#endif

///////////////////////////////////////////////////////////////////////////////
namespace boost {
namespace wave {
namespace util {

///////////////////////////////////////////////////////////////////////////////
// retrieve the macro name from the parse tree
template <typename ParseNodeT, typename TokenT, typename PositionT>
inline void  
retrieve_macroname(ParseNodeT const &node, boost::spirit::parser_id id, 
    TokenT &macroname, PositionT const &act_pos)
{
ParseNodeT const *name_node = 0;

    using boost::spirit::find_node;
    if (!find_node(node, id, &name_node)) 
    {
        // ill formed define statement (unexpected, should not happen)
        BOOST_WAVE_THROW(preprocess_exception, bad_define_statement, 
            "bad parse tree (unexpected)", act_pos);
    }
    
typename ParseNodeT::children_t const &children = name_node->children;

    if (0 == children.size() || 
        children.front().value.begin() == children.front().value.end()) 
    {
        // ill formed define statement (unexpected, should not happen)
        BOOST_WAVE_THROW(preprocess_exception, bad_define_statement, 
            "bad parse tree (unexpected)", act_pos);
    }

// retrieve the macro name
    macroname = *children.front().value.begin();
}

///////////////////////////////////////////////////////////////////////////////
// retrieve the macro parameters or the macro definition from the parse tree
template <typename ParseNodeT, typename TokenT, typename ContainerT>
inline bool  
retrieve_macrodefinition(
    ParseNodeT const &node, boost::spirit::parser_id id, 
    ContainerT &macrodefinition, TokenT const &/*t*/)
{
    using namespace boost::wave;
    typedef typename ParseNodeT::const_tree_iterator const_tree_iterator;

// find macro parameters/macro definition inside the parse tree
std::pair<const_tree_iterator, const_tree_iterator> nodes;

    using boost::spirit::get_node_range;
    if (get_node_range(node, id, nodes)) {
    // copy all parameters to the supplied container
        typename ContainerT::iterator last_nonwhite = macrodefinition.end();
        const_tree_iterator end = nodes.second;
        
        for (const_tree_iterator cit = nodes.first; cit != end; ++cit) {
            if ((*cit).value.begin() != (*cit).value.end()) {
            typename ContainerT::iterator inserted = macrodefinition.insert(
                macrodefinition.end(), *(*cit).value.begin());
                
                if (!IS_CATEGORY(macrodefinition.back(), WhiteSpaceTokenType) &&
                    T_NEWLINE != token_id(macrodefinition.back()) &&
                    T_EOF != token_id(macrodefinition.back()))
                {
                    last_nonwhite = inserted;
                }
            }
        }
        
    // trim trailing whitespace (leading whitespace is trimmed by the grammar)
        if (last_nonwhite != macrodefinition.end()) {
            macrodefinition.erase(++last_nonwhite, macrodefinition.end());
        }
        return true;
    }
    return false;
}

#if BOOST_WAVE_ENABLE_COMMANDLINE_MACROS != 0
///////////////////////////////////////////////////////////////////////////////
//  add an additional predefined macro given by a string (MACRO(x)=definition)
template <typename ContextT>
bool add_macro_definition(ContextT &ctx, std::string macrostring,
    bool is_predefined, boost::wave::language_support language)
{
    typedef typename ContextT::token_type token_type;
    typedef typename ContextT::lexer_type lexer_type;
    typedef typename token_type::position_type position_type;
    typedef boost::wave::grammars::predefined_macros_grammar_gen<lexer_type> 
        predef_macros_type;

    using namespace boost::wave;
    using namespace std;    // isspace is in std namespace for some systems
    
// skip leading whitespace
std::string::iterator begin = macrostring.begin();
std::string::iterator end = macrostring.end();

    while(begin != end && isspace(*begin))
        ++begin;
        
// parse the macro definition
position_type act_pos("<command line>", 0);
boost::spirit::tree_parse_info<lexer_type> hit = 
    predef_macros_type::parse_predefined_macro(
        lexer_type(begin, end, position_type(), language), lexer_type());

    if (!hit.match || (!hit.full && T_EOF != token_id(*hit.stop))) {
        BOOST_WAVE_THROW(preprocess_exception, bad_macro_definition, 
            macrostring.c_str(), act_pos);
    }
    
// retrieve the macro definition from the parse tree
token_type macroname;
std::vector<token_type> macroparameters;
typename ContextT::token_sequence_type macrodefinition;
bool has_parameters = false;

    boost::wave::util::retrieve_macroname(*hit.trees.begin(), 
        BOOST_WAVE_PLAIN_DEFINE_ID, macroname, act_pos);
    has_parameters = boost::wave::util::retrieve_macrodefinition(*hit.trees.begin(), 
        BOOST_WAVE_MACRO_PARAMETERS_ID, macroparameters, token_type());
    boost::wave::util::retrieve_macrodefinition(*hit.trees.begin(), 
        BOOST_WAVE_MACRO_DEFINITION_ID, macrodefinition, token_type());

//  If no macrodefinition is given, and the macro string does not end with a 
//  '=', then the macro should be defined with the value '1'
    if (0 == macrodefinition.size() && 
        '=' != macrostring[macrostring.size()-1])
    {
        macrodefinition.push_back(token_type(T_INTLIT, "1", act_pos));
    }
    
// add the new macro to the macromap
    return ctx.add_macro_definition(macroname, has_parameters, macroparameters, 
        macrodefinition, is_predefined);
}
#endif // BOOST_WAVE_ENABLE_COMMANDLINE_MACROS != 0

///////////////////////////////////////////////////////////////////////////////
}   // namespace util

///////////////////////////////////////////////////////////////////////////////
//  forward declaration
template <typename ContextT> class pp_iterator;

namespace impl {

///////////////////////////////////////////////////////////////////////////////
//  
//  pp_iterator_functor
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
class pp_iterator_functor {

public:
// interface to the boost::spirit::multi_pass_policies::functor_input policy
    typedef typename ContextT::token_type               result_type;

//  eof token
    static result_type const eof;

private:
// type of a token sequence
    typedef typename ContextT::token_sequence_type      token_sequence_type;

    typedef typename ContextT::lexer_type               lexer_type;
    typedef typename result_type::string_type           string_type;
    typedef typename result_type::position_type         position_type;
    typedef boost::wave::grammars::cpp_grammar_gen<lexer_type, token_sequence_type> 
        cpp_grammar_type;

//  iteration context related types (an iteration context represents a current
//  position in an included file)
    typedef base_iteration_context<lexer_type>    base_iteration_context_type;
    typedef 
        iteration_context<lexer_type, typename ContextT::input_policy_type>
        iteration_context_type;

// parse tree related types
    typedef typename cpp_grammar_type::node_factory_type node_factory_type;
    typedef boost::spirit::tree_parse_info<lexer_type, node_factory_type> 
        tree_parse_info_type;
    typedef boost::spirit::tree_match<lexer_type, node_factory_type> 
        parse_tree_match_type;
    typedef typename parse_tree_match_type::node_t       parse_node_type;       // tree_node<node_val_data<> >
    typedef typename parse_tree_match_type::parse_node_t parse_node_value_type; // node_val_data<>
    typedef typename parse_tree_match_type::container_t  parse_tree_type;       // parse_node_type::children_t

public:
    template <typename IteratorT>
    pp_iterator_functor(ContextT &ctx_, IteratorT const &first_, 
            IteratorT const &last_, typename ContextT::position_type const &pos_)
    :   ctx(ctx_), 
        iter_ctx(new base_iteration_context_type(
                lexer_type(first_, last_, pos_, 
                    boost::wave::enable_prefer_pp_numbers(ctx.get_language())), 
                lexer_type(), 
                pos_.get_file().c_str()
            )), 
        seen_newline(true), must_emit_line_directive(false),
        act_pos(ctx_.get_main_pos())
    {
        act_pos.set_file(pos_.get_file());
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
        ctx_.set_current_filename(pos_.get_file().c_str());
#endif
    }
    
// get the next preprocessed token
    result_type const &operator()();

// get the last recognized token (for error processing etc.)
    result_type const &current_token() const { return act_token; }

protected:
    friend class pp_iterator<ContextT>;
    void on_include_helper(char const *t, char const *s, bool is_system, 
        bool include_next);
    
protected:
    result_type const &get_next_token();
    result_type const &pp_token(bool consider_emitting_line_directive = false);

    bool pp_directive();
    template <typename IteratorT>
    bool can_ignore_pp_directive(IteratorT &it);
    bool dispatch_directive(tree_parse_info_type const &hit,
        result_type const& found_directive,
        token_sequence_type const& found_eoltokens);

    void on_include(string_type const &s, bool is_system, bool include_next);
    void on_include(typename parse_tree_type::const_iterator const &begin,
        typename parse_tree_type::const_iterator const &end, bool include_next);

    void on_define(parse_node_type const &node);
    void on_undefine(result_type const &t);
    
    void on_ifdef(typename parse_tree_type::const_iterator const &begin,
        typename parse_tree_type::const_iterator const &end);
    void on_ifndef(typename parse_tree_type::const_iterator const &begin,
        typename parse_tree_type::const_iterator const &end);
    void on_else();
    void on_endif();
    void on_illformed(typename result_type::string_type s);
        
    void on_line(typename parse_tree_type::const_iterator const &begin,
        typename parse_tree_type::const_iterator const &end);
    void on_if(typename parse_tree_type::const_iterator const &begin,
        typename parse_tree_type::const_iterator const &end);
    void on_elif(typename parse_tree_type::const_iterator const &begin,
        typename parse_tree_type::const_iterator const &end,
        token_sequence_type const& found_eoltokens);
    void on_error(typename parse_tree_type::const_iterator const &begin,
        typename parse_tree_type::const_iterator const &end);
#if BOOST_WAVE_SUPPORT_WARNING_DIRECTIVE != 0
    void on_warning(typename parse_tree_type::const_iterator const &begin,
        typename parse_tree_type::const_iterator const &end);
#endif
    bool on_pragma(typename parse_tree_type::const_iterator const &begin,
        typename parse_tree_type::const_iterator const &end);

    bool emit_line_directive();
    bool returned_from_include();

    bool interpret_pragma(token_sequence_type const &pragma_body,
        token_sequence_type &result);

private:
    ContextT &ctx;              // context, this iterator is associated with
    boost::shared_ptr<base_iteration_context_type> iter_ctx;
    
    bool seen_newline;              // needed for recognizing begin of line
    bool must_emit_line_directive;  // must emit a line directive
    result_type act_token;          // current token
    typename result_type::position_type &act_pos;   // current fileposition (references the macromap)
        
    token_sequence_type unput_queue;     // tokens to be preprocessed again
    token_sequence_type pending_queue;   // tokens already preprocessed
    
    // detect whether to insert additional whitespace in between two adjacent 
    // tokens, which otherwise would form a different token type, if 
    // retokenized
    boost::wave::util::insert_whitespace_detection whitespace; 
};

///////////////////////////////////////////////////////////////////////////////
//  eof token
template <typename ContextT>
typename pp_iterator_functor<ContextT>::result_type const
    pp_iterator_functor<ContextT>::eof;

///////////////////////////////////////////////////////////////////////////////
//
//  returned_from_include()
// 
//      Tests if it is necessary to pop the include file context (eof inside
//      a file was reached). If yes, it pops this context. Preprocessing will
//      continue with the next outer file scope.
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline bool 
pp_iterator_functor<ContextT>::returned_from_include()
{
    if (iter_ctx->first == iter_ctx->last && ctx.get_iteration_depth() > 0) {
    // call the include policy trace function
        ctx.get_hooks().returning_from_include_file();
        
    // restore the previous iteration context after finishing the preprocessing 
    // of the included file
        BOOST_WAVE_STRINGTYPE oldfile = iter_ctx->real_filename;
        position_type old_pos (act_pos);
        
    // if this file has include guards handle it as if it had a #pragma once
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
        if (need_include_guard_detection(ctx.get_language())) {
            std::string guard_name;
            if (iter_ctx->first.has_include_guards(guard_name))
                ctx.add_pragma_once_header(ctx.get_current_filename(), guard_name);
        }
#endif 
        iter_ctx = ctx.pop_iteration_context();

        must_emit_line_directive = true;
        seen_newline = true;

    // restore current file position
        act_pos.set_file(iter_ctx->filename);
        act_pos.set_line(iter_ctx->line);
        act_pos.set_column(0);
        
    // restore the actual current file and directory 
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
        ctx.set_current_filename(iter_ctx->real_filename.c_str());
#endif 
        ctx.set_current_directory(iter_ctx->real_filename.c_str());

    // ensure the integrity of the #if/#endif stack
    // report unbalanced #if/#endif now to make it possible to recover properly
        if (iter_ctx->if_block_depth != ctx.get_if_block_depth()) {
            using boost::wave::util::impl::escape_lit;
            BOOST_WAVE_THROW(preprocess_exception, unbalanced_if_endif, 
                escape_lit(oldfile).c_str(), old_pos);
        }
        return true;
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////
//
//  operator()(): get the next preprocessed token
//
//      throws a preprocess_exception, if appropriate
//
///////////////////////////////////////////////////////////////////////////////
namespace impl {

    //  It may be necessary to emit a #line directive either 
    //  - when comments need to be preserved: if the current token is not a 
    //    whitespace, except comments
    //  - when comments are to be skipped: if the current token is not a 
    //    whitespace token.
    template <typename ContextT> 
    bool consider_emitting_line_directive(ContextT const& ctx, token_id id)
    {
        if (need_preserve_comments(ctx.get_language())) {
            if (!IS_CATEGORY(id, EOLTokenType) && !IS_CATEGORY(id, EOFTokenType))
            {
                return true;
            }
        }
        if (!IS_CATEGORY(id, WhiteSpaceTokenType) && 
            !IS_CATEGORY(id, EOLTokenType) && !IS_CATEGORY(id, EOFTokenType))
        {
          return true;
        }
        return false;
    }
}

template <typename ContextT> 
inline typename pp_iterator_functor<ContextT>::result_type const &
pp_iterator_functor<ContextT>::operator()()
{
    using namespace boost::wave;

// loop over skip able whitespace until something significant is found
bool skipped_newline = false;
bool was_seen_newline = seen_newline;
token_id id = T_UNKNOWN;
    
    do {
    // get_next_token assigns result to act_token member
        if (!seen_newline && skipped_newline)
            seen_newline = true;
        get_next_token();

    // if comments shouldn't be preserved replace them with newlines
        id = token_id(act_token);
        if (!need_preserve_comments(ctx.get_language()) &&
            (T_CPPCOMMENT == id || context_policies::util::ccomment_has_newline(act_token)))
        {
            act_token.set_token_id(id = T_NEWLINE);
            act_token.set_value("\n");
        }
        
    } while (ctx.get_hooks().may_skip_whitespace(ctx, act_token, skipped_newline));
    
// if there were skipped any newlines, we must emit a #line directive
    if ((must_emit_line_directive || (was_seen_newline && skipped_newline)) && 
        impl::consider_emitting_line_directive(ctx, id)) 
    {
    // must emit a #line directive
        if (need_emit_line_directives(ctx.get_language()) && emit_line_directive()) 
        {
            skipped_newline = false;
            ctx.get_hooks().may_skip_whitespace(ctx, act_token, skipped_newline);     // feed ws eater FSM
            id = token_id(act_token);
        }
    }
    
// cleanup of certain tokens required
    seen_newline = false;
    switch (static_cast<unsigned int>(id)) {
    case T_NONREPLACABLE_IDENTIFIER:
        act_token.set_token_id(T_IDENTIFIER);
        id = T_IDENTIFIER;
        break;

    case T_GENERATEDNEWLINE:  // was generated by emit_line_directive()
        act_token.set_token_id(id = T_NEWLINE);
        ++iter_ctx->emitted_lines;
        seen_newline = true;
        break;
        
    case T_NEWLINE:
    case T_CPPCOMMENT:
        seen_newline = true;
        ++iter_ctx->emitted_lines;
        break;

    case T_CCOMMENT:          // will come here only if whitespace is preserved
        iter_ctx->emitted_lines += 
            context_policies::util::ccomment_count_newlines(act_token);
        break;
        
    case T_PP_NUMBER:        // re-tokenize the pp-number
        {
            token_sequence_type rescanned;
            
            std::string pp_number(act_token.get_value().c_str());
            lexer_type it = lexer_type(pp_number.begin(), 
                pp_number.end(), act_token.get_position(), 
                ctx.get_language());
            lexer_type end = lexer_type();
            for (/**/; it != end && T_EOF != token_id(*it); ++it) 
                rescanned.push_back(*it);
                
            pending_queue.splice(pending_queue.begin(), rescanned);
            act_token = pending_queue.front();
            id = token_id(act_token);
            pending_queue.pop_front();
        }
        break;
        
    case T_EOF:
        seen_newline = true;
        break;

    default:    // make sure whitespace at line begin keeps seen_newline status
        if (IS_CATEGORY(id, WhiteSpaceTokenType))
            seen_newline = skipped_newline;
        break;
    }

    if (whitespace.must_insert(id, act_token.get_value())) {
    // must insert some whitespace into the output stream to avoid adjacent
    // tokens, which would form different (and wrong) tokens
        whitespace.shift_tokens(T_SPACE);
        pending_queue.push_front(act_token);        // push this token back
        return act_token = result_type(T_SPACE, 
            typename result_type::string_type(" "), 
            act_token.get_position());
    }
    whitespace.shift_tokens(id);
    return act_token;
}


template <typename ContextT> 
inline typename pp_iterator_functor<ContextT>::result_type const &
pp_iterator_functor<ContextT>::get_next_token()
{
    using namespace boost::wave;
    
// if there is something in the unput_queue, then return the next token from
// there (all tokens in the queue are preprocessed already)
    if (!pending_queue.empty() || !unput_queue.empty()) 
        return pp_token();      // return next token
    
// test for EOF, if there is a pending input context, pop it back and continue
// parsing with it
bool returned_from_include_file = returned_from_include();
    
// try to generate the next token 
    if (iter_ctx->first != iter_ctx->last) {
        do {
        // If there are pending tokens in the queue, we'll have to return 
        // these. This may happen from a #pragma directive, which got replaced
        // by some token sequence.
            if (!pending_queue.empty()) {
            util::on_exit::pop_front<token_sequence_type> 
                pop_front_token(pending_queue);

                whitespace.shift_tokens(act_token = pending_queue.front());
                return act_token;
            }
            
        // fetch the current token        
            act_token = *iter_ctx->first;

        // adjust the current position (line and column)
        bool was_seen_newline = seen_newline || returned_from_include_file;

            act_pos = act_token.get_position();
            
        // act accordingly on the current token
        token_id id = token_id(act_token);
        
            if (T_EOF == id) {
            // returned from an include file, continue with the next token
                whitespace.shift_tokens(T_EOF);
                ++iter_ctx->first;

            // now make sure this line has a newline
                if ((!seen_newline || act_pos.get_column() > 1) && 
                    !(support_option_single_line & get_support_options(ctx.get_language()))) 
                {
                // warn, if this file does not end with a newline
                    BOOST_WAVE_THROW(preprocess_exception, 
                        last_line_not_terminated, "", act_pos);
                }
                continue;   // if this is the main file, the while loop breaks
            }
            else if (T_NEWLINE == id || T_CPPCOMMENT == id) {   
            // a newline is to be returned ASAP, a C++ comment too
            // (the C++ comment token includes the trailing newline)
                seen_newline = true;
                ++iter_ctx->first;
                whitespace.shift_tokens(id);  // whitespace controller
                
                if (!ctx.get_if_block_status()) {
                // skip this token because of the disabled #if block
                    ctx.get_hooks().skipped_token(act_token);
                    continue;
                }
                return act_token; 
            }
            seen_newline = false;

            if (was_seen_newline && pp_directive()) {
            // a pp directive was found
//                 seen_newline = true;
//                 must_emit_line_directive = true;

            // loop to the next token to analyze
            // simply fall through, since the iterator was already adjusted 
            // correctly
            }
            else if (ctx.get_if_block_status()) {
            // preprocess this token, eat up more, if appropriate, return 
            // the next preprocessed token
                return pp_token(was_seen_newline);
            }
            else {
            // compilation condition is false: if the current token is a 
            // newline, account for it, otherwise discard the actual token and 
            // try the next one
                if (T_NEWLINE == act_token) {
                    seen_newline = true;
                    must_emit_line_directive = true;
                }

            // next token
                ctx.get_hooks().skipped_token(act_token);
                ++iter_ctx->first;
            }
            
        } while ((iter_ctx->first != iter_ctx->last) || 
                 (returned_from_include_file = returned_from_include()));

    // overall eof reached
        if (ctx.get_if_block_depth() > 0 &&
            !(support_option_single_line & get_support_options(ctx.get_language()))) 
        {
        // missing endif directive(s)
            BOOST_WAVE_THROW(preprocess_exception, missing_matching_endif, "", 
                act_pos);
        }
    }
    else {
        act_token = eof;            // this is the last token
    }
    
    whitespace.shift_tokens(T_EOF);     // whitespace controller
    return act_token;                   // return eof token
}

///////////////////////////////////////////////////////////////////////////////
//
//  emit_line_directive(): emits a line directive from the act_token data
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline bool
pp_iterator_functor<ContextT>::emit_line_directive()
{
    using namespace boost::wave;
    
typename ContextT::position_type pos = act_token.get_position();

    if (must_emit_line_directive || 
        iter_ctx->emitted_lines != act_pos.get_line()) 
    {
    // unput the current token
        pending_queue.push_front(act_token);
        pos.set_line(act_pos.get_line());

        if (!must_emit_line_directive &&
            iter_ctx->emitted_lines+1 == act_pos.get_line()) 
        {
        // prefer to output a single newline instead of the #line directive
            whitespace.shift_tokens(T_NEWLINE);
            act_token = result_type(T_NEWLINE, "\n", pos);
        }
        else {
        // account for the newline emitted here
            act_pos.set_line(act_pos.get_line()-1);
            iter_ctx->emitted_lines = act_pos.get_line();
        
        // the #line directive has to be pushed back into the pending queue in 
        // reverse order

        // unput the complete #line directive in reverse order
        std::string file("\"");
        boost::filesystem::path filename(act_pos.get_file().c_str(), 
            boost::filesystem::native);
        
            using boost::wave::util::impl::escape_lit;
            file += escape_lit(filename.native_file_string()) + "\"";

        // 21 is the max required size for a 64 bit integer represented as a 
        // string
        char buffer[22];

            using namespace std;    // for some systems sprintf is in namespace std
            sprintf (buffer, "%d", pos.get_line());

        // adjust the generated column numbers accordingly
        // #line<space>number<space>filename<newline>
        unsigned int filenamelen = (unsigned int)file.size();
        unsigned int column = 7 + (unsigned int)strlen(buffer) + filenamelen;

            pos.set_line(pos.get_line() - 1);         // adjust line number
            
            pos.set_column(column);
            pending_queue.push_front(result_type(T_GENERATEDNEWLINE, "\n", pos));
            pos.set_column(column -= filenamelen);    // account for filename
            pending_queue.push_front(result_type(T_STRINGLIT, file.c_str(), pos));
            pos.set_column(--column);                 // account for ' '
            pending_queue.push_front(result_type(T_SPACE, " ", pos));
            pos.set_column(column -= (unsigned int)strlen(buffer)); // account for <number>
            pending_queue.push_front(result_type(T_INTLIT, buffer, pos));
            pos.set_column(--column);                 // account for ' '
            pending_queue.push_front(result_type(T_SPACE, " ", pos));
            
        // return the #line token itself
            whitespace.shift_tokens(T_PP_LINE);
            pos.set_column(1);
            act_token = result_type(T_PP_LINE, "#line", pos);
        }
        
        must_emit_line_directive = false;     // we are now in sync
        return true;
    }

    must_emit_line_directive = false;         // we are now in sync
    return false;
}

///////////////////////////////////////////////////////////////////////////////
//
//  pptoken(): return the next preprocessed token
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline typename pp_iterator_functor<ContextT>::result_type const &
pp_iterator_functor<ContextT>::pp_token(bool consider_emitting_line_directive)
{
    using namespace boost::wave;

token_id id = token_id(*iter_ctx->first);

    // eat all T_PLACEHOLDER tokens, eventually slipped through out of the
    // macro engine
    do { 
        if (!pending_queue.empty()) {
        // if there are pending tokens in the queue, return the first one
            act_token = pending_queue.front();
            pending_queue.pop_front();
            act_pos = act_token.get_position();
        }
        else if (!unput_queue.empty()
              || T_IDENTIFIER == id 
              || IS_CATEGORY(id, KeywordTokenType)
              || IS_EXTCATEGORY(id, OperatorTokenType|AltExtTokenType)
              || IS_CATEGORY(id, BoolLiteralTokenType))
        {
        //  call the lexer, preprocess the required number of tokens, put them
        //  into the unput queue
            act_token = ctx.expand_tokensequence(iter_ctx->first, 
                iter_ctx->last, pending_queue, unput_queue);
        }
        else {
        // simply return the next token
            act_token = *iter_ctx->first;
            ++iter_ctx->first;
        }
        id = token_id(act_token);
        
    } while (T_PLACEHOLDER == id);
    return act_token;
}

///////////////////////////////////////////////////////////////////////////////
//
//  pp_directive(): recognize a preprocessor directive
//
///////////////////////////////////////////////////////////////////////////////
namespace impl {

    template <typename ContexT, typename IteratorT>
    bool next_token_is_pp_directive(ContexT &ctx, IteratorT &it, IteratorT const &end)
    {
        using namespace boost::wave;
        
        token_id id = T_UNKNOWN;
        for (/**/; it != end; ++it) {
            id = token_id(*it);
            if (!IS_CATEGORY(id, WhiteSpaceTokenType))
                break;          // skip leading whitespace
            if (IS_CATEGORY(id, EOLTokenType))
                break;          // do not enter a new line
                
            ctx.get_hooks().skipped_token(*it);   // this token get's skipped
        }
        BOOST_ASSERT(it == end || id != T_UNKNOWN);
        return it != end && IS_CATEGORY(id, PPTokenType);
    }
    
    template <typename ContexT, typename IteratorT>
    bool pp_is_last_on_line(ContexT &ctx, IteratorT &it, IteratorT const &end)
    {
        using namespace boost::wave;
        
        ctx.get_hooks().skipped_token(*it);     // this token get's skipped

        for (++it; it != end; ++it) {
            token_id id = token_id(*it);
            if (T_CPPCOMMENT == id || T_NEWLINE == id ||
                context_policies::util::ccomment_has_newline(*it)) 
            {
                ctx.get_hooks().skipped_token(*it);
                ++it;           // skip eol/C/C++ comment
                return true;    // no more significant tokens on this line
            }

            if (!IS_CATEGORY(id, WhiteSpaceTokenType))
                break;

            ctx.get_hooks().skipped_token(*it);   // this token get's skipped
        }
        return false;
    }

    template <typename ContexT, typename IteratorT>
    bool skip_to_eol(ContexT &ctx, IteratorT &it, IteratorT const &end)
    {
        using namespace boost::wave;
        
        for (/**/; it != end; ++it) {
        token_id id = token_id(*it);
        
            ctx.get_hooks().skipped_token(*it);
            if (T_CPPCOMMENT == id || T_NEWLINE == id ||
                context_policies::util::ccomment_has_newline(*it)) 
            {
                ++it;           // skip eol/C/C++ comment
                return true;    // found eol
            }
        }
        return false;
    }
}

///////////////////////////////////////////////////////////////////////////////
//  can_ignore_pp_directive: handle certain pp_directives if if_block_status is 
//                           false
template <typename ContextT> 
template <typename IteratorT>
inline bool
pp_iterator_functor<ContextT>::can_ignore_pp_directive(IteratorT &it)
{
    bool can_exit = true;
    if (IS_EXTCATEGORY(*it, PPConditionalTokenType)) {
    // simulate the if block hierarchy
        switch (static_cast<unsigned int>(token_id(*it))) {
        case T_PP_IFDEF:        // #ifdef
        case T_PP_IFNDEF:       // #ifndef
        case T_PP_IF:           // #if
            ctx.enter_if_block(false);
            break;

        case T_PP_ELIF:         // #elif
            if (!ctx.get_enclosing_if_block_status()) {
                if (!ctx.enter_elif_block(false)) { 
                // #else without matching #if
                    BOOST_WAVE_THROW(preprocess_exception, 
                        missing_matching_if, "#elif", act_pos);
                }
            }
            else {
                can_exit = false;   // #elif is not always safe to skip
            }
            break;

        case T_PP_ELSE:         // #else
        case T_PP_ENDIF:        // #endif
            {
            // handle this directive
                if (T_PP_ELSE == token_id(*it))
                    on_else();
                else
                    on_endif();

            // make sure, there are no (non-whitespace) tokens left on this line                
                string_type value ((*it).get_value());
                if (!impl::pp_is_last_on_line(ctx, it, iter_ctx->last)) {
                // enable error recovery (start over with the next line)
                    impl::skip_to_eol(ctx, it, iter_ctx->last);
                    seen_newline = true;
                    iter_ctx->first = it;
                
                // report an invalid #else directive
                    on_illformed(value);
                    break;
                }

            // we skipped to the end of this line already
                seen_newline = true;
                iter_ctx->first = it;
            }
            return true;
              
        default:                // #something else
            on_illformed((*it).get_value());
            break;
        }
    }

// start over with the next line, if only possible
    if (can_exit) {
        string_type value ((*it).get_value());
        if (!impl::skip_to_eol(ctx, it, iter_ctx->last)) {
        // The line doesn't end with an eol but eof token.
            seen_newline = true;    // allow to resume after warning
            iter_ctx->first = it;
            
        // Trigger a warning, that the last line was not terminated with a 
        // newline.
            BOOST_WAVE_THROW(preprocess_exception, last_line_not_terminated, 
                "", act_pos);
        }
        return true;    // may be safely ignored
    }
    
    return false;   // do not ignore this pp directive
}

///////////////////////////////////////////////////////////////////////////////
//  pp_directive(): recognize a preprocessor directive
template <typename ContextT> 
inline bool
pp_iterator_functor<ContextT>::pp_directive()
{
    using namespace cpplexer;
    
// test, if the next non-whitespace token is a pp directive
lexer_type it = iter_ctx->first;

    if (!impl::next_token_is_pp_directive(ctx, it, iter_ctx->last)) {
    // eventually skip null pp directive (no need to do it via the parser)
        if (it != iter_ctx->last && T_POUND == BASE_TOKEN(token_id(*it))) {
            if (impl::pp_is_last_on_line(ctx, it, iter_ctx->last)) {
            // start over with the next line
                seen_newline = true;
                iter_ctx->first = it;
                return true;
            }
            else if (ctx.get_if_block_status()) {
            // report invalid pp directive
                on_illformed((*it).get_value());  
            }
        }
        
    // this line does not contain a pp directive, so simply return
        return false;
    }

// found eof
    if (it == iter_ctx->last)
        return false;

// ignore/handle all pp directives not related to conditional compilation while
// if block status is false
    if (!ctx.get_if_block_status() && can_ignore_pp_directive(it)) {
    // we may skip pp directives only, if the current if block status is false
        seen_newline = true;
        iter_ctx->first = it;
        return true;    //  the pp directive was handled/skipped
    }
    
// found a pp directive, so try to identify it, start with the pp_token
bool found_eof = false;
result_type found_directive;
token_sequence_type found_eoltokens;

tree_parse_info_type hit = cpp_grammar_type::parse_cpp_grammar(
    it, iter_ctx->last, act_pos, found_eof, found_directive, found_eoltokens);

    if (hit.match) {
    // position the iterator past the matched sequence to allow 
    // resynchronization, if an error occurs
        iter_ctx->first = hit.stop;
        seen_newline = true;
        must_emit_line_directive = true;

    // found a valid pp directive, dispatch to the correct function to handle 
    // the found pp directive
    bool result = dispatch_directive (hit, found_directive, found_eoltokens);
    
        if (found_eof) {
        // The line was terminated with an end of file token.
        // So trigger a warning, that the last line was not terminated with a 
        // newline.
            BOOST_WAVE_THROW(preprocess_exception, last_line_not_terminated, "", 
                act_pos);
        }
        return result;
    }
    else if (token_id(found_directive) != T_EOF) {
    // recognized invalid directive
        impl::skip_to_eol(ctx, it, iter_ctx->last);
        seen_newline = true;
        
        string_type str(boost::wave::util::impl::as_string<string_type>(
            iter_ctx->first, it));
        iter_ctx->first = it;

    // report the ill formed directive
        on_illformed(str);
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////
//
//  dispatch_directive(): dispatch a recognized preprocessor directive
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline bool
pp_iterator_functor<ContextT>::dispatch_directive(
    tree_parse_info_type const &hit, result_type const& found_directive,
    token_sequence_type const& found_eoltokens)
{
    using namespace cpplexer;
    using namespace boost::spirit;
    
    typedef typename parse_tree_type::const_iterator const_child_iterator_t;
    
// this iterator points to the root node of the parse tree
const_child_iterator_t begin = hit.trees.begin();

// decide, which preprocessor directive was found
parse_tree_type const &root = (*begin).children;
parse_node_value_type const &nodeval = get_first_leaf(*root.begin()).value;
//long node_id = nodeval.id().to_long();

const_child_iterator_t begin_child_it = (*root.begin()).children.begin();
const_child_iterator_t end_child_it = (*root.begin()).children.end();

token_id id = token_id(found_directive);

    // call preprocessing hook
    ctx.get_hooks().found_directive(found_directive);     
    
    switch (static_cast<unsigned int>(id)) {
    case T_PP_QHEADER:      // #include "..."
#if BOOST_WAVE_SUPPORT_INCLUDE_NEXT != 0
    case T_PP_QHEADER_NEXT: // #include_next "..."
#endif 
        on_include ((*nodeval.begin()).get_value(), false, 
            T_PP_QHEADER_NEXT == id);
        break;

    case T_PP_HHEADER:      // #include <...>
#if BOOST_WAVE_SUPPORT_INCLUDE_NEXT != 0
    case T_PP_HHEADER_NEXT: // #include_next <...>
#endif 
        on_include ((*nodeval.begin()).get_value(), true, 
            T_PP_HHEADER_NEXT == id);
        break;
    
    case T_PP_INCLUDE:      // #include ...
#if BOOST_WAVE_SUPPORT_INCLUDE_NEXT != 0
    case T_PP_INCLUDE_NEXT: // #include_next ...
#endif 
        on_include (begin_child_it, end_child_it, T_PP_INCLUDE_NEXT == id);
        break;

    case T_PP_DEFINE:       // #define
        on_define (*begin);
        break;

    case T_PP_UNDEF:        // #undef
        on_undefine(*nodeval.begin());
        break;

    case T_PP_IFDEF:        // #ifdef
        on_ifdef(begin_child_it, end_child_it);
        break;

    case T_PP_IFNDEF:       // #ifndef
        on_ifndef(begin_child_it, end_child_it);
        break;

    case T_PP_IF:           // #if
        on_if(begin_child_it, end_child_it);
        break;

    case T_PP_ELIF:         // #elif
        on_elif(begin_child_it, end_child_it, found_eoltokens);
        break;

    case T_PP_ELSE:         // #else
        on_else();
        break;

    case T_PP_ENDIF:        // #endif
        on_endif();
        break;

    case T_PP_LINE:         // #line
        on_line(begin_child_it, end_child_it);
        break;
        
    case T_PP_ERROR:        // #error
        on_error(begin_child_it, end_child_it);
        break;

#if BOOST_WAVE_SUPPORT_WARNING_DIRECTIVE != 0
    case T_PP_WARNING:      // #warning
        on_warning(begin_child_it, end_child_it);
        break;
#endif 

    case T_PP_PRAGMA:       // #pragma
        return on_pragma(begin_child_it, end_child_it);

#if BOOST_WAVE_SUPPORT_MS_EXTENSIONS != 0
    case T_MSEXT_PP_REGION:
    case T_MSEXT_PP_ENDREGION:
        break;              // ignore these
#endif

    default:                // #something else
        on_illformed((*nodeval.begin()).get_value());
        break;
    }
    return true;    // return newline only
}

///////////////////////////////////////////////////////////////////////////////
// 
//  on_include: handle #include <...> or #include "..." directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_include (string_type const &s, 
    bool is_system, bool include_next) 
{
    BOOST_ASSERT(ctx.get_if_block_status());

// strip quotes first, extract filename
typename string_type::size_type pos_end = s.find_last_of(is_system ? '>' : '\"');

    if (string_type::npos == pos_end) {
        BOOST_WAVE_THROW(preprocess_exception, bad_include_statement, 
            s.c_str(), act_pos);
    }

typename string_type::size_type pos_begin = 
    s.find_last_of(is_system ? '<' : '\"', pos_end-1);

    if (string_type::npos == pos_begin) {
        BOOST_WAVE_THROW(preprocess_exception, bad_include_statement, 
            s.c_str(), act_pos);
    }

std::string file_token(s.substr(pos_begin, pos_end-pos_begin+1).c_str());
std::string file_path(s.substr(pos_begin+1, pos_end-pos_begin-1).c_str());

// finally include the file
    on_include_helper(file_token.c_str(), file_path.c_str(), is_system, 
        include_next);
}
       
template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_include_helper (char const *f, char const *s, 
    bool is_system, bool include_next) 
{
    namespace fs = boost::filesystem;

// try to locate the given file, searching through the include path lists
std::string file_path(s);
std::string dir_path;
#if BOOST_WAVE_SUPPORT_INCLUDE_NEXT != 0
char const *current_name = include_next ? iter_ctx->real_filename.c_str() : 0;
#else
char const *current_name = 0;   // never try to match current file name
#endif

// call the include policy trace function
    ctx.get_hooks().found_include_directive(f, include_next);

    file_path = util::impl::unescape_lit(file_path);
    if (!ctx.find_include_file (file_path, dir_path, is_system, current_name)) {
        BOOST_WAVE_THROW(preprocess_exception, bad_include_file, 
            file_path.c_str(), act_pos);
    }

fs::path native_path(file_path, fs::native);

    if (!fs::exists(native_path)) {
        BOOST_WAVE_THROW(preprocess_exception, bad_include_file, 
            file_path.c_str(), act_pos);
    }

// test, if this file is known through a #pragma once directive
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
    if (!ctx.has_pragma_once(native_path.native_file_string())) 
#endif 
    {
    // the new include file determines the actual current directory
        ctx.set_current_directory(native_path.native_file_string().c_str());
        
    // preprocess the opened file
    boost::shared_ptr<base_iteration_context_type> new_iter_ctx (
        new iteration_context_type(native_path.native_file_string().c_str(), 
            act_pos, boost::wave::enable_prefer_pp_numbers(ctx.get_language())));

    // call the include policy trace function
        ctx.get_hooks().opened_include_file(dir_path, file_path,
            ctx.get_iteration_depth(), is_system);

    // store current file position
        iter_ctx->filename = act_pos.get_file();
        iter_ctx->line = act_pos.get_line();
        iter_ctx->if_block_depth = ctx.get_if_block_depth();
        
    // push the old iteration context onto the stack and continue with the new
        ctx.push_iteration_context(act_pos, iter_ctx);
        iter_ctx = new_iter_ctx;
        seen_newline = true;        // fake a newline to trigger pp_directive
        must_emit_line_directive = true;
        
        act_pos.set_file(iter_ctx->filename);  // initialize file position
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
        ctx.set_current_filename(iter_ctx->real_filename.c_str());
#endif 

        act_pos.set_line(iter_ctx->line);
        act_pos.set_column(0);
    }
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_include(): handle #include ... directives
//
///////////////////////////////////////////////////////////////////////////////

namespace impl {

    // trim all whitespace from the beginning and the end of the given string
    template <typename StringT>
    inline StringT 
    trim_whitespace(StringT const &s)
    {
        typedef typename StringT::size_type size_type;
        
        size_type first = s.find_first_not_of(" \t\v\f");
        if (StringT::npos == first)
            return StringT();
        size_type last = s.find_last_not_of(" \t\v\f");
        return s.substr(first, last-first+1);
    }
}

template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_include(
    typename parse_tree_type::const_iterator const &begin,
    typename parse_tree_type::const_iterator const &end, bool include_next)
{
    BOOST_ASSERT(ctx.get_if_block_status());

// preprocess the given token sequence (the body of the #include directive)
get_token_value<result_type, parse_node_type> get_value;
token_sequence_type expanded;
token_sequence_type toexpand;

    std::copy(make_ref_transform_iterator(begin, get_value), 
        make_ref_transform_iterator(end, get_value),
        std::inserter(toexpand, toexpand.end()));

    typename token_sequence_type::iterator begin2 = toexpand.begin();
    ctx.expand_whole_tokensequence(begin2, toexpand.end(), expanded, 
        false);

// now, include the file
string_type s (impl::trim_whitespace(boost::wave::util::impl::as_string(expanded)));
bool is_system = '<' == s[0] && '>' == s[s.size()-1];

    if (!is_system && !('\"' == s[0] && '\"' == s[s.size()-1])) {
    // should resolve into something like <...> or "..."
        BOOST_WAVE_THROW(preprocess_exception, bad_include_statement, 
            s.c_str(), act_pos);
    }
    on_include(s, is_system, include_next);
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_define(): handle #define directives
//
///////////////////////////////////////////////////////////////////////////////

template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_define (parse_node_type const &node) 
{
    BOOST_ASSERT(ctx.get_if_block_status());

// retrieve the macro definition from the parse tree
result_type macroname;
std::vector<result_type> macroparameters;
token_sequence_type macrodefinition;
bool has_parameters = false;

    boost::wave::util::retrieve_macroname(node, 
        BOOST_WAVE_PLAIN_DEFINE_ID, macroname, 
        act_token.get_position());
    has_parameters = boost::wave::util::retrieve_macrodefinition(node, 
        BOOST_WAVE_MACRO_PARAMETERS_ID, macroparameters, act_token);
    boost::wave::util::retrieve_macrodefinition(node, 
        BOOST_WAVE_MACRO_DEFINITION_ID, macrodefinition, act_token);

    if (has_parameters) {
#if BOOST_WAVE_SUPPORT_VARIADICS_PLACEMARKERS != 0
        if (boost::wave::need_variadics(ctx.get_language())) {
        // test whether ellipsis are given, and if yes, if these are placed as the
        // last argument, test if __VA_ARGS__ is used as a macro parameter name
            using namespace cpplexer;
            typedef typename std::vector<result_type>::iterator 
                parameter_iterator_t;
            
            bool seen_ellipses = false;
            parameter_iterator_t end = macroparameters.end();
            for (parameter_iterator_t pit = macroparameters.begin(); 
                pit != end; ++pit) 
            {
                if (seen_ellipses) {
                // ellipses are not the last given formal argument
                    BOOST_WAVE_THROW(preprocess_exception, bad_define_statement, 
                        macroname.get_value().c_str(), (*pit).get_position());
                }
                if (T_ELLIPSIS == token_id(*pit)) 
                    seen_ellipses = true;

                // can't use __VA_ARGS__ as a argument name
                if ("__VA_ARGS__" == (*pit).get_value()) {
                    BOOST_WAVE_THROW(preprocess_exception, bad_define_statement_va_args, 
                        macroname.get_value().c_str(), (*pit).get_position());
                }
            }
            
        // if there wasn't an ellipsis, then there shouldn't be a __VA_ARGS__ 
        // placeholder in the definition too [C99 Standard 6.10.3.5]
            if (!seen_ellipses) {
                typedef typename token_sequence_type::iterator definition_iterator_t;

                bool seen_va_args = false;
                definition_iterator_t pend = macrodefinition.end();
                for (definition_iterator_t dit = macrodefinition.begin(); 
                     dit != pend; ++dit) 
                {
                    if (T_IDENTIFIER == token_id(*dit) && 
                        "__VA_ARGS__" == (*dit).get_value())
                    {
                        seen_va_args = true;
                    }
                }
                if (seen_va_args) {
                // must not have seen __VA_ARGS__ placeholder
                    BOOST_WAVE_THROW(preprocess_exception, bad_define_statement_va_args, 
                        macroname.get_value().c_str(), act_token.get_position());
                }
            }
        }
        else
#endif // BOOST_WAVE_SUPPORT_VARIADICS_PLACEMARKERS != 0
        {
        // test, that there is no T_ELLIPSES given
            using namespace cpplexer;
            typedef typename std::vector<result_type>::iterator 
                parameter_iterator_t;
            
            parameter_iterator_t end = macroparameters.end();
            for (parameter_iterator_t pit = macroparameters.begin(); 
                pit != end; ++pit) 
            {
                if (T_ELLIPSIS == token_id(*pit)) {
                // if variadics are disabled, no ellipses should be given
                    BOOST_WAVE_THROW(preprocess_exception, bad_define_statement, 
                        macroname.get_value().c_str(), (*pit).get_position());
                }
            }
        }
    }
    
// add the new macro to the macromap
    ctx.add_macro_definition(macroname, has_parameters, macroparameters, 
        macrodefinition);
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_undefine(): handle #undef directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_undefine (result_type const &token) 
{
    BOOST_ASSERT(ctx.get_if_block_status());

// retrieve the macro name to undefine from the parse tree
    ctx.remove_macro_definition(token.get_value()); // throws for predefined macros
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_ifdef(): handle #ifdef directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_ifdef(
    typename parse_tree_type::const_iterator const &begin,
    typename parse_tree_type::const_iterator const &end)
{
get_token_value<result_type, parse_node_type> get_value;
token_sequence_type toexpand;

    std::copy(make_ref_transform_iterator((*begin).children.begin(), get_value), 
        make_ref_transform_iterator((*begin).children.end(), get_value),
        std::inserter(toexpand, toexpand.end()));

bool is_defined = ctx.is_defined_macro(toexpand.begin(), toexpand.end());

    ctx.get_hooks().evaluated_conditional_expression(toexpand, is_defined);
    ctx.enter_if_block(is_defined);
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_ifndef(): handle #ifndef directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_ifndef(
    typename parse_tree_type::const_iterator const &begin,
    typename parse_tree_type::const_iterator const &end)
{
get_token_value<result_type, parse_node_type> get_value;
token_sequence_type toexpand;

    std::copy(make_ref_transform_iterator((*begin).children.begin(), get_value), 
        make_ref_transform_iterator((*begin).children.end(), get_value),
        std::inserter(toexpand, toexpand.end()));

bool is_defined = ctx.is_defined_macro(toexpand.begin(), toexpand.end());

    ctx.get_hooks().evaluated_conditional_expression(toexpand, is_defined);
    ctx.enter_if_block(!is_defined);
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_else(): handle #else directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_else()
{
    if (!ctx.enter_else_block()) {
    // #else without matching #if
        BOOST_WAVE_THROW(preprocess_exception, missing_matching_if, "#else", 
            act_pos);
    }
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_endif(): handle #endif directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_endif()
{
    if (!ctx.exit_if_block()) {
    // #endif without matching #if
        BOOST_WAVE_THROW(preprocess_exception, missing_matching_if, "#endif", 
            act_pos);
    }
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_if(): handle #if directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_if(
    typename parse_tree_type::const_iterator const &begin,
    typename parse_tree_type::const_iterator const &end)
{
// preprocess the given sequence into the provided list
get_token_value<result_type, parse_node_type> get_value;
token_sequence_type expanded;
token_sequence_type toexpand;

    std::copy(make_ref_transform_iterator(begin, get_value), 
        make_ref_transform_iterator(end, get_value),
        std::inserter(toexpand, toexpand.end()));

    typename token_sequence_type::iterator begin2 = toexpand.begin();
    ctx.expand_whole_tokensequence(begin2, toexpand.end(), expanded);

// replace all remaining (== undefined) identifiers with an integer literal '0'
    typename token_sequence_type::iterator exp_end = expanded.end();
    for (typename token_sequence_type::iterator exp_it = expanded.begin();
         exp_it != exp_end; ++exp_it)
    {
        using namespace boost::wave;
        
        token_id id = token_id(*exp_it);
        if (IS_CATEGORY(id, IdentifierTokenType) ||
            IS_CATEGORY(id, KeywordTokenType))
        {
            (*exp_it).set_token_id(T_INTLIT);
            (*exp_it).set_value("0");
        }
    }
    
#if BOOST_WAVE_DUMP_CONDITIONAL_EXPRESSIONS != 0
    {
        string_type outstr(boost::wave::util::impl::as_string(toexpand));
        outstr += "(" + boost::wave::util::impl::as_string(expanded) + ")";
        BOOST_WAVE_DUMP_CONDITIONAL_EXPRESSIONS_OUT << "#if " << outstr 
            << std::endl;
    }
#endif

// parse the expression and enter the #if block
grammars::value_error status = grammars::error_noerror;
bool if_status = grammars::expression_grammar_gen<result_type>::
            evaluate(expanded.begin(), expanded.end(), act_pos,
                ctx.get_if_block_status(), status);
                
    ctx.get_hooks().evaluated_conditional_expression(toexpand, if_status);
    if (grammars::error_noerror != status) {
    // division or other error by zero occurred
        string_type expression = util::impl::as_string(expanded);
        if (0 == expression.size()) 
            expression = "<empty expression>";
            
        if (grammars::error_division_by_zero & status) {
            BOOST_WAVE_THROW(preprocess_exception, division_by_zero, 
                expression.c_str(), act_pos);
        }
        if (grammars::error_integer_overflow & status) {
        // we may validly continue 
            ctx.enter_if_block(if_status);
            BOOST_WAVE_THROW(preprocess_exception, integer_overflow, 
                expression.c_str(), act_pos);
            return;
        }
        if (grammars::error_character_overflow & status) {
        // we may validly continue 
            ctx.enter_if_block(if_status);
            BOOST_WAVE_THROW(preprocess_exception, 
                character_literal_out_of_range, expression.c_str(), act_pos);
            return;
        }
    }

    ctx.enter_if_block(if_status);
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_elif(): handle #elif directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_elif(
    typename parse_tree_type::const_iterator const &begin,
    typename parse_tree_type::const_iterator const &end,
    token_sequence_type const& found_eoltokens)
{
// preprocess the given sequence into the provided list
get_token_value<result_type, parse_node_type> get_value;
token_sequence_type toexpand;

    std::copy(make_ref_transform_iterator(begin, get_value), 
        make_ref_transform_iterator(end, get_value),
        std::inserter(toexpand, toexpand.end()));

// check current if block status
    if (ctx.get_if_block_some_part_status()) {
        if (!ctx.enter_elif_block(false)) {
        // #else without matching #if
            BOOST_WAVE_THROW(preprocess_exception, missing_matching_if, "#elif", 
                act_pos);
        }

    // skip all the expression and the trailing whitespace
    typename token_sequence_type::iterator begin2 = toexpand.begin();
    typename token_sequence_type::const_iterator begin3 = found_eoltokens.begin();

        impl::skip_to_eol(ctx, begin2, toexpand.end());
        impl::skip_to_eol(ctx, begin3, found_eoltokens.end());
        return;     // one of previous #if/#elif was true, so don't enter this #elif 
    }
            
// preprocess the given sequence into the provided list
token_sequence_type expanded;

    typename token_sequence_type::iterator begin2 = toexpand.begin();
    ctx.expand_whole_tokensequence(begin2, toexpand.end(), expanded);
    
// replace all remaining (== undefined) identifiers with an integer literal '0'
    typename token_sequence_type::iterator exp_end = expanded.end();
    for (typename token_sequence_type::iterator exp_it = expanded.begin();
         exp_it != exp_end; ++exp_it)
    {
        using namespace boost::wave;
        
        token_id id = token_id(*exp_it);
        if (IS_CATEGORY(id, IdentifierTokenType) ||
            IS_CATEGORY(id, KeywordTokenType))
        {
            (*exp_it).set_token_id(T_INTLIT);
            (*exp_it).set_value("0");
        }
    }

#if BOOST_WAVE_DUMP_CONDITIONAL_EXPRESSIONS != 0
    {
        string_type outstr(boost::wave::util::impl::as_string(toexpand));
        outstr += "(" + boost::wave::util::impl::as_string(expanded) + ")";
        BOOST_WAVE_DUMP_CONDITIONAL_EXPRESSIONS_OUT << "#elif " << outstr << std::endl;
    }
#endif

// parse the expression and enter the #elif block
grammars::value_error status = grammars::error_noerror;
bool if_status = grammars::expression_grammar_gen<result_type>::
            evaluate(expanded.begin(), expanded.end(), act_pos,
                ctx.get_if_block_status(), status);
                
    ctx.get_hooks().evaluated_conditional_expression(toexpand, if_status);
    if (grammars::error_noerror != status) {
    // division or other error by zero occurred
        string_type expression = util::impl::as_string(expanded);
        if (0 == expression.size()) 
            expression = "<empty expression>";
            
        if (grammars::error_division_by_zero & status) {
            BOOST_WAVE_THROW(preprocess_exception, division_by_zero, 
                expression.c_str(), act_pos);
        }
        if (grammars::error_integer_overflow & status) {
        // we validly may continue
            if (!ctx.enter_elif_block(if_status)) { 
            // #elif without matching #if
                BOOST_WAVE_THROW(preprocess_exception, missing_matching_if, 
                    "#elif", act_pos);
            }
            BOOST_WAVE_THROW(preprocess_exception, integer_overflow,
                expression.c_str(), act_pos);
            return;
        }
        if (grammars::error_character_overflow & status) {
        // we validly may continue
            if (!ctx.enter_elif_block(if_status)) { 
            // #elif without matching #if
                BOOST_WAVE_THROW(preprocess_exception, missing_matching_if, 
                    "#elif", act_pos);
            }
            BOOST_WAVE_THROW(preprocess_exception, 
                character_literal_out_of_range, expression.c_str(), act_pos);
            return;
        }
    }

    if (!ctx.enter_elif_block(if_status)) { 
    // #elif without matching #if
        BOOST_WAVE_THROW(preprocess_exception, missing_matching_if, "#elif", 
            act_pos);
    }
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_illformed(): handles the illegal directive
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_illformed(
    typename result_type::string_type s)
{
    BOOST_ASSERT(ctx.get_if_block_status());
    
    // some messages have more than one newline at the end
    typename string_type::size_type p = s.find_last_not_of('\n');
    if (string_type::npos != p)
        s = s.substr(0, p+1);

    // throw the exception
    BOOST_WAVE_THROW(preprocess_exception, ill_formed_directive, s.c_str(), 
        act_pos);
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_line(): handle #line directives
//
///////////////////////////////////////////////////////////////////////////////

namespace impl {

    template <typename IteratorT, typename StringT>
    bool retrieve_line_info (IteratorT first, IteratorT const &last,
        int &line, StringT &file, 
        boost::wave::preprocess_exception::error_code& error)
    {
        using namespace boost::wave;
        token_id id = token_id(*first);
        if (T_PP_NUMBER == id || T_INTLIT == id) {
        // extract line number
            using namespace std;    // some systems have atoi in namespace std
            line = (unsigned int)atoi((*first).get_value().c_str());
            if (0 == line)
                error = preprocess_exception::bad_line_number;

        // re-extract line number with spirit to diagnose overflow
            using namespace boost::spirit;
            if (!parse((*first).get_value().c_str(), int_p).full)
                error = preprocess_exception::bad_line_number;
                
        // extract file name (if it is given)
            while (++first != last && IS_CATEGORY(*first, WhiteSpaceTokenType)) 
                /**/;   // skip whitespace
                
            if (first != last) {
                if (T_STRINGLIT != token_id(*first)) {
                    error = preprocess_exception::bad_line_filename;
                    return false;
                }
                
            StringT const &file_lit = (*first).get_value();
            
                if ('L' == file_lit[0]) {
                    error = preprocess_exception::bad_line_filename;
                    return false;       // shouldn't be a wide character string 
                }
                
                file = file_lit.substr(1, file_lit.size()-2);

            // test if there is other junk on this line
                while (++first != last && IS_CATEGORY(*first, WhiteSpaceTokenType)) 
                    /**/;   // skip whitespace
            }
            return first == last;
        }
        error = preprocess_exception::bad_line_statement;
        return false;
    }
}

template <typename ContextT> 
inline void  
pp_iterator_functor<ContextT>::on_line(
    typename parse_tree_type::const_iterator const &begin,
    typename parse_tree_type::const_iterator const &end)
{
    BOOST_ASSERT(ctx.get_if_block_status());

// Try to extract the line number and file name from the given token list
// directly. If that fails, preprocess the whole token sequence and try again 
// to extract this information.
token_sequence_type expanded;
get_token_value<result_type, parse_node_type> get_value;

    typedef typename ref_transform_iterator_generator<
            get_token_value<result_type, parse_node_type>, 
            typename parse_tree_type::const_iterator
        >::type const_tree_iterator_t;
        
const_tree_iterator_t first = make_ref_transform_iterator(begin, get_value);
const_tree_iterator_t last = make_ref_transform_iterator(end, get_value);
    
// try to interpret the #line body as a number followed by an optional
// string literal
int line = 0;
preprocess_exception::error_code error = preprocess_exception::no_error;
string_type file_name;

    if (!impl::retrieve_line_info(first, last, line, file_name, error)) {
    // preprocess the body of this #line message
    token_sequence_type toexpand;

        std::copy(first, make_ref_transform_iterator(end, get_value),
            std::inserter(toexpand, toexpand.end()));

        typename token_sequence_type::iterator begin2 = toexpand.begin();
        ctx.expand_whole_tokensequence(begin2, toexpand.end(), 
            expanded, false);

        error = preprocess_exception::no_error;
        if (!impl::retrieve_line_info(expanded.begin(), expanded.end(), 
            line, file_name, error))
        {
            BOOST_WAVE_THROW_VAR(preprocess_exception, error, 
                boost::wave::util::impl::as_string(expanded).c_str(), act_pos)
        }
    }
    
// the queues should be empty at this point
    BOOST_ASSERT(unput_queue.empty());
    BOOST_ASSERT(pending_queue.empty());

// make sure error recovery starts on the next line
    must_emit_line_directive = true;
    
// diagnose possible error in detected line directive
    if (error != preprocess_exception::no_error) {
        BOOST_WAVE_THROW_VAR(preprocess_exception, error, 
            boost::wave::util::impl::as_string(expanded).c_str(), act_pos)
    }    

// set new line number/filename only if ok
    if (!file_name.empty()) {    // reuse current file name 
        using boost::wave::util::impl::unescape_lit;
        act_pos.set_file(unescape_lit(file_name).c_str());
    }
    act_pos.set_line(line);
    iter_ctx->first.set_position(act_pos);
}

///////////////////////////////////////////////////////////////////////////////
//  
//  on_error(): handle #error directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void 
pp_iterator_functor<ContextT>::on_error(
    typename parse_tree_type::const_iterator const &begin,
    typename parse_tree_type::const_iterator const &end)
{
    BOOST_ASSERT(ctx.get_if_block_status());

// preprocess the given sequence into the provided list
token_sequence_type expanded;
get_token_value<result_type, parse_node_type> get_value;

typename ref_transform_iterator_generator<
        get_token_value<result_type, parse_node_type>, 
        typename parse_tree_type::const_iterator
    >::type first = make_ref_transform_iterator(begin, get_value);
    
#if BOOST_WAVE_PREPROCESS_ERROR_MESSAGE_BODY != 0
// preprocess the body of this #error message
token_sequence_type toexpand;

    std::copy(first, make_ref_transform_iterator(end, get_value),
        std::inserter(toexpand, toexpand.end()));

    typename token_sequence_type::iterator begin2 = toexpand.begin();
    ctx.expand_whole_tokensequence(begin2, toexpand.end(), expanded, 
        false);
#else
// simply copy the body of this #error message to the issued diagnostic
// message
    std::copy(first, make_ref_transform_iterator(end, get_value), 
        std::inserter(expanded, expanded.end()));
#endif 

// report the corresponding error
    BOOST_WAVE_THROW(preprocess_exception, error_directive, 
        boost::wave::util::impl::as_string(expanded).c_str(), act_pos);
}

#if BOOST_WAVE_SUPPORT_WARNING_DIRECTIVE != 0
///////////////////////////////////////////////////////////////////////////////
//  
//  on_warning(): handle #warning directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline void 
pp_iterator_functor<ContextT>::on_warning(
    typename parse_tree_type::const_iterator const &begin,
    typename parse_tree_type::const_iterator const &end)
{
    BOOST_ASSERT(ctx.get_if_block_status());

// preprocess the given sequence into the provided list
token_sequence_type expanded;
get_token_value<result_type, parse_node_type> get_value;

typename ref_transform_iterator_generator<
        get_token_value<result_type, parse_node_type>, 
        typename parse_tree_type::const_iterator
    >::type first = make_ref_transform_iterator(begin, get_value);
    
#if BOOST_WAVE_PREPROCESS_ERROR_MESSAGE_BODY != 0
// preprocess the body of this #warning message
token_sequence_type toexpand;

    std::copy(first, make_ref_transform_iterator(end, get_value),
        std::inserter(toexpand, toexpand.end()));

    typename token_sequence_type::iterator begin2 = toexpand.begin();
    ctx.expand_whole_tokensequence(begin2, toexpand.end(), expanded, 
        false);
#else
// simply copy the body of this #warning message to the issued diagnostic
// message
    std::copy(first, make_ref_transform_iterator(end, get_value), 
        std::inserter(expanded, expanded.end()));
#endif 

// report the corresponding error
    BOOST_WAVE_THROW(preprocess_exception, warning_directive, 
        boost::wave::util::impl::as_string(expanded).c_str(), act_pos);
}
#endif // BOOST_WAVE_SUPPORT_WARNING_DIRECTIVE != 0

///////////////////////////////////////////////////////////////////////////////
//  
//  on_pragma(): handle #pragma directives
//
///////////////////////////////////////////////////////////////////////////////
template <typename ContextT> 
inline bool
pp_iterator_functor<ContextT>::on_pragma(
    typename parse_tree_type::const_iterator const &begin,
    typename parse_tree_type::const_iterator const &end)
{
    using namespace boost::wave;
    
    BOOST_ASSERT(ctx.get_if_block_status());

// Look at the pragma token sequence and decide, if the first token is STDC
// (see C99 standard [6.10.6.2]), if it is, the sequence must _not_ be
// preprocessed.
token_sequence_type expanded;
get_token_value<result_type, parse_node_type> get_value;

    typedef typename ref_transform_iterator_generator<
            get_token_value<result_type, parse_node_type>, 
            typename parse_tree_type::const_iterator
        >::type const_tree_iterator_t;
        
const_tree_iterator_t first = make_ref_transform_iterator(begin, get_value);
const_tree_iterator_t last = make_ref_transform_iterator(end, get_value);

    expanded.push_back(result_type(T_PP_PRAGMA, "#pragma", act_token.get_position()));
    expanded.push_back(result_type(T_SPACE, " ", act_token.get_position()));
        
    while (++first != last && IS_CATEGORY(*first, WhiteSpaceTokenType)) 
        expanded.push_back(*first);   // skip whitespace

    if (first != last) {
        if (T_IDENTIFIER == token_id(*first) && 
            boost::wave::need_c99(ctx.get_language()) && 
            (*first).get_value() == "STDC") 
        {
        // do _not_ preprocess the token sequence
            std::copy(first, last, std::inserter(expanded, expanded.end()));
        }
        else {
#if BOOST_WAVE_PREPROCESS_PRAGMA_BODY != 0
        // preprocess the given tokensequence
        token_sequence_type toexpand;

            std::copy(first, last, std::inserter(toexpand, toexpand.end()));

            typename token_sequence_type::iterator begin2 = toexpand.begin();
            ctx.expand_whole_tokensequence(begin2, toexpand.end(), 
                expanded, false);
#else
        // do _not_ preprocess the token sequence
            std::copy(first, last, std::inserter(expanded, expanded.end()));
#endif
        }
    }
    expanded.push_back(result_type(T_NEWLINE, "\n", act_token.get_position()));
        
// the queues should be empty at this point
    BOOST_ASSERT(unput_queue.empty());
    BOOST_ASSERT(pending_queue.empty());

// try to interpret the expanded #pragma body 
    token_sequence_type pending;
    if (interpret_pragma(expanded, pending)) {
    // if there is some replacement text, insert it into the pending queue
        if (pending.size() > 0)
            pending_queue.splice(pending_queue.begin(), pending);
        return true;        // this #pragma was successfully recognized
    }
    
#if BOOST_WAVE_EMIT_PRAGMA_DIRECTIVES != 0
// Move the resulting token sequence into the pending_queue, so it will be 
// returned to the caller.
    pending_queue.splice(pending_queue.begin(), expanded);
    return false;           // return the whole #pragma directive
#else
    return true;            // skip the #pragma at all
#endif 
}

template <typename ContextT> 
inline bool
pp_iterator_functor<ContextT>::interpret_pragma(
    token_sequence_type const &pragma_body, token_sequence_type &result)
{
    using namespace cpplexer;
    
    typename token_sequence_type::const_iterator end = pragma_body.end();
    typename token_sequence_type::const_iterator it = pragma_body.begin();
    for (++it; it != end && IS_CATEGORY(*it, WhiteSpaceTokenType); ++it) 
        /**/;   // skip whitespace
    
    if (it == end)      // eof reached
        return false;

    return boost::wave::util::interpret_pragma(ctx, act_token, it, end, result);
}

///////////////////////////////////////////////////////////////////////////////
}   // namespace impl

///////////////////////////////////////////////////////////////////////////////
//  
//  pp_iterator
//
//      The boost::wave::pp_iterator template is the iterator, through which
//      the resulting preprocessed input stream is accessible.
//  
///////////////////////////////////////////////////////////////////////////////

template <typename ContextT>
class pp_iterator 
:   public boost::spirit::multi_pass<
        boost::wave::impl::pp_iterator_functor<ContextT>,
        boost::wave::util::functor_input
    >
{
public:
    typedef boost::wave::impl::pp_iterator_functor<ContextT> input_policy_type;

private:
    typedef 
        boost::spirit::multi_pass<input_policy_type, boost::wave::util::functor_input>
        base_type;
    typedef pp_iterator<ContextT> self_type;
    typedef boost::wave::util::functor_input functor_input_type;
    
public:
    pp_iterator() 
    {}
    
    template <typename IteratorT>
    pp_iterator(ContextT &ctx, IteratorT const &first, IteratorT const &last,
        typename ContextT::position_type const &pos)
    :   base_type(input_policy_type(ctx, first, last, pos))
    {}
    
    void force_include(char const *path_, bool is_last)
    { 
        this->get_functor().on_include_helper(path_, path_, false, false); 
        if (is_last) {
            this->functor_input_type::
                template inner<input_policy_type>::advance_input();
        }
    }
};

///////////////////////////////////////////////////////////////////////////////
}   // namespace wave
}   // namespace boost

// the suffix header occurs after all of the code
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_SUFFIX
#endif

#endif // !defined(CPP_ITERATOR_HPP_175CA88F_7273_43FA_9039_BCF7459E1F29_INCLUDED)
