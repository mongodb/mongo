/*=============================================================================
    Boost.Wave: A Standard compliant C++ preprocessor library
    Definition of the preprocessor context
    
    http://www.boost.org/

    Copyright (c) 2001-2007 Hartmut Kaiser. Distributed under the Boost
    Software License, Version 1.0. (See accompanying file
    LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#if !defined(CPP_CONTEXT_HPP_907485E2_6649_4A87_911B_7F7225F3E5B8_INCLUDED)
#define CPP_CONTEXT_HPP_907485E2_6649_4A87_911B_7F7225F3E5B8_INCLUDED

#include <string>
#include <vector>
#include <stack>

#include <boost/concept_check.hpp>
#include <boost/noncopyable.hpp>
#include <boost/filesystem/path.hpp>

#include <boost/wave/wave_config.hpp>
#if BOOST_WAVE_SERIALIZATION != 0
#include <boost/serialization/serialization.hpp>
#include <boost/wave/wave_config_constant.hpp>
#endif
#include <boost/wave/token_ids.hpp>

#include <boost/wave/util/unput_queue_iterator.hpp>
#include <boost/wave/util/cpp_ifblock.hpp>
#include <boost/wave/util/cpp_include_paths.hpp>
#include <boost/wave/util/iteration_context.hpp>
#include <boost/wave/util/cpp_iterator.hpp>
#include <boost/wave/util/cpp_macromap.hpp>

#include <boost/wave/preprocessing_hooks.hpp>
#include <boost/wave/whitespace_handling.hpp>
#include <boost/wave/cpp_iteration_context.hpp>
#include <boost/wave/language_support.hpp>

// this must occur after all of the includes and before any code appears
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_PREFIX
#endif

///////////////////////////////////////////////////////////////////////////////
namespace boost {
namespace wave {

///////////////////////////////////////////////////////////////////////////////
// 
//  The C preprocessor context template class
//
//      The boost::wave::context template is the main interface class to 
//      control the behaviour of the preprocessing engine.
//
//      The following template parameters has to be supplied:
//
//      IteratorT       The iterator type of the underlying input stream
//      LexIteratorT    The lexer iterator type to use as the token factory
//      InputPolicyT    The input policy type to use for loading the files
//                      to be included. This template parameter is optional and 
//                      defaults to the 
//                          iteration_context_policies::load_file_to_string
//                      type.
//      HooksT          The hooks policy to use for different notification 
//                      callbacks. This template parameter is optional and
//                      defaults to the
//                          context_policies::default_preprocessing_hooks
//                      type.
//
///////////////////////////////////////////////////////////////////////////////

template <
    typename IteratorT,
    typename LexIteratorT, 
    typename InputPolicyT = iteration_context_policies::load_file_to_string,
    typename HooksT = context_policies::eat_whitespace<typename LexIteratorT::token_type>
>
class context : private boost::noncopyable
{
public:

// concept checks
// the given iterator shall be at least a forward iterator type
    BOOST_CLASS_REQUIRE(IteratorT, boost, ForwardIteratorConcept);
    
// public typedefs
    typedef typename LexIteratorT::token_type       token_type;
    typedef context<IteratorT, LexIteratorT, InputPolicyT, HooksT> 
        self_type;
    
    typedef IteratorT                               target_iterator_type;
    typedef LexIteratorT                            lexer_type;
    typedef pp_iterator<self_type>                  iterator_type;

    typedef InputPolicyT                            input_policy_type;
    typedef typename token_type::position_type      position_type;

    
// type of a token sequence
    typedef std::list<token_type, boost::fast_pool_allocator<token_type> > 
        token_sequence_type;
// types of the policies
    typedef HooksT                                  hook_policy_type;
    
private:
// stack of shared_ptr's to the pending iteration contexts 
    typedef boost::shared_ptr<base_iteration_context<lexer_type> > 
        iteration_ptr_type;
    typedef boost::wave::util::iteration_context_stack<iteration_ptr_type> 
            iteration_context_stack_type;
    typedef typename iteration_context_stack_type::size_type iter_size_type;

    context *this_() { return this; }           // avoid warning in constructor
    
public:
    context(target_iterator_type const &first_, target_iterator_type const &last_, 
            char const *fname = "<Unknown>", HooksT const &hooks_ = HooksT())
    :   first(first_), last(last_), filename(fname)
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
        , current_filename(fname)
#endif 
        , macros(*this_())
        , language(language_support(
                      support_cpp 
                    | support_option_convert_trigraphs 
                    | support_option_emit_line_directives 
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
                    | support_option_include_guard_detection
#endif
                   ))
        , hooks(hooks_)
    {
        macros.init_predefined_macros(fname);
        includes.init_initial_path();
    }
    
// default copy constructor
// default assignment operator
// default destructor
    
// iterator interface
    iterator_type begin() 
    { 
        std::string fname(filename);
        if (filename != "<Unknown>" && filename != "<stdin>") {
            using namespace boost::filesystem;
            path fpath(complete(path(filename)));
            fname = fpath.string();
            includes.set_current_directory(fname.c_str());
        }
        return iterator_type(*this, first, last, position_type(fname.c_str())); 
    }
    iterator_type begin(
        target_iterator_type const &first_, 
        target_iterator_type const &last_) 
    { 
        std::string fname(filename);
        if (filename != "<Unknown>" && filename != "<stdin>") {
            using namespace boost::filesystem;
            path fpath(complete(path(filename)));
            fname = fpath.string();
            includes.set_current_directory(fname.c_str());
        }
        return iterator_type(*this, first_, last_, position_type(fname.c_str())); 
    }
    iterator_type end() const 
        { return iterator_type(); }

// maintain include paths
    bool add_include_path(char const *path_)
        { return includes.add_include_path(path_, false);}
    bool add_sysinclude_path(char const *path_)
        { return includes.add_include_path(path_, true);}
    void set_sysinclude_delimiter() { includes.set_sys_include_delimiter(); }
    typename iteration_context_stack_type::size_type get_iteration_depth() const 
        { return iter_ctxs.size(); }

// maintain defined macros
#if BOOST_WAVE_ENABLE_COMMANDLINE_MACROS != 0
    bool add_macro_definition(std::string macrostring, 
            bool is_predefined = false)
        { return boost::wave::util::add_macro_definition(*this, macrostring, 
            is_predefined, get_language()); }
#endif 
    bool add_macro_definition(token_type const &name, bool has_params,
            std::vector<token_type> &parameters, token_sequence_type &definition,
            bool is_predefined = false)
        { return macros.add_macro(name, has_params, parameters, definition, 
            is_predefined); }
    template <typename IteratorT2>
    bool is_defined_macro(IteratorT2 const &begin, IteratorT2 const &end) 
        { return macros.is_defined(begin, end); }
    bool get_macro_definition(typename token_type::string_type const &name, 
            bool &has_params, bool &is_predefined, position_type &pos,
            std::vector<token_type> &parameters, token_sequence_type &definition)
        { 
            return macros.get_macro(name, has_params, is_predefined, pos,
                parameters, definition); 
        }
    bool remove_macro_definition(typename token_type::string_type const &name, 
            bool even_predefined = false)
        { 
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
            // ensure this gets remove from the list of include guards as well
            includes.remove_pragma_once_header(std::string(name.c_str()));
#endif
            return macros.remove_macro(
                token_type(T_IDENTIFIER, name, macros.get_main_pos()), 
                even_predefined); 
        }
    void reset_macro_definitions() 
        { macros.reset_macromap(); macros.init_predefined_macros(); }

// get the Wave version information 
    static std::string get_version()  
        { return boost::wave::util::predefined_macros::get_fullversion(false); }
    static std::string get_version_string()  
        { return boost::wave::util::predefined_macros::get_versionstr(false); }

// access current language options
    void set_language(boost::wave::language_support language_,
                      bool reset_macros = true) 
    { 
        language = language_; 
        if (reset_macros)
            reset_macro_definitions();
    }
    boost::wave::language_support get_language() const { return language; }
        
// change and ask for maximal possible include nesting depth
    void set_max_include_nesting_depth(iter_size_type new_depth)
        { iter_ctxs.set_max_include_nesting_depth(new_depth); }
    iter_size_type get_max_include_nesting_depth() const
        { return iter_ctxs.get_max_include_nesting_depth(); }

// access the policies
    hook_policy_type &get_hooks() { return hooks; }

// return the directory of the currently preprocessed file
    boost::filesystem::path get_current_directory() const
        { return includes.get_current_directory(); }
        
#if !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
protected:
    friend class boost::wave::pp_iterator<
        boost::wave::context<IteratorT, lexer_type, InputPolicyT, HooksT> >;
    friend class boost::wave::impl::pp_iterator_functor<
        boost::wave::context<IteratorT, lexer_type, InputPolicyT, HooksT> >;
#endif
    
// maintain include paths (helper functions)
    bool find_include_file (std::string &s, std::string &d, bool is_system, 
        char const *current_file) const
    { return includes.find_include_file(s, d, is_system, current_file); }
    void set_current_directory(char const *path_) 
        { includes.set_current_directory(path_); }
        
// conditional compilation contexts
    bool get_if_block_status() const { return ifblocks.get_status(); }
    bool get_if_block_some_part_status() const 
        { return ifblocks.get_some_part_status(); } 
    bool get_enclosing_if_block_status() const
        { return ifblocks.get_enclosing_status(); }
    void enter_if_block(bool new_status) 
        { ifblocks.enter_if_block(new_status); }
    bool enter_elif_block(bool new_status) 
        { return ifblocks.enter_elif_block(new_status); }
    bool enter_else_block() { return ifblocks.enter_else_block(); }
    bool exit_if_block() { return ifblocks.exit_if_block(); }
    typename boost::wave::util::if_block_stack::size_type get_if_block_depth() const 
        { return ifblocks.get_if_block_depth(); }

// stack of iteration contexts
    iteration_ptr_type pop_iteration_context()
        { iteration_ptr_type top = iter_ctxs.top(); iter_ctxs.pop(); return top; }
    void push_iteration_context(position_type const &act_pos, iteration_ptr_type iter_ctx)
        { iter_ctxs.push(act_pos, iter_ctx); }

    position_type &get_main_pos() { return macros.get_main_pos(); }
    
///////////////////////////////////////////////////////////////////////////////
//
//  expand_tokensequence(): 
//      expands all macros contained in a given token sequence, handles '##' 
//      and '#' pp operators and re-scans the resulting sequence 
//      (essentially pre-processes the token sequence).
//
//      The expand_undefined parameter is true during macro expansion inside
//      a C++ expression given for a #if or #elif statement. 
//
///////////////////////////////////////////////////////////////////////////////
    template <typename IteratorT2>
    token_type expand_tokensequence(IteratorT2 &first_, IteratorT2 const &last_, 
        token_sequence_type &pending, token_sequence_type &expanded, 
        bool expand_undefined = false)
    {
        return macros.expand_tokensequence(first_, last_, pending, expanded, 
            expand_undefined);
    }

    template <typename IteratorT2>
    void expand_whole_tokensequence(IteratorT2 &first_, IteratorT2 const &last_, 
        token_sequence_type &expanded, bool expand_undefined = true)
    {
        macros.expand_whole_tokensequence(expanded, first_, last_, 
            expand_undefined);

    // remove any contained placeholder
        boost::wave::util::impl::remove_placeholders(expanded);
    }

public:
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
// support for #pragma once
// maintain the real name of the current preprocessed file
    void set_current_filename(char const *real_name)
        { current_filename = real_name; }
    std::string const &get_current_filename() const 
        { return current_filename; }

// maintain the list of known headers containing #pragma once 
    bool has_pragma_once(std::string const &filename_)
        { return includes.has_pragma_once(filename_); }
    bool add_pragma_once_header(std::string const &filename_,
            std::string const& guard_name = "__BOOST_WAVE_PRAGMA_ONCE__")
        { return includes.add_pragma_once_header(filename_, guard_name); }
#endif 

// forwarding functions for the context policy hooks    
    template <typename ContainerT>
    bool interpret_pragma(ContainerT &pending, token_type const &option, 
        ContainerT const &values, token_type const &act_token)
    {
        return hooks.interpret_pragma(*this, pending, option, values, act_token);
    }
    
#if BOOST_WAVE_SERIALIZATION != 0
public:
    BOOST_STATIC_CONSTANT(unsigned int, version = 0x10);
    BOOST_STATIC_CONSTANT(unsigned int, version_mask = 0x0f);

private:
    friend class boost::serialization::access;
    template<class Archive>
    void save(Archive & ar, const unsigned int version) const
    {
        typedef typename token_type::string_type string_type;
        
        string_type cfg(BOOST_PP_STRINGIZE(BOOST_WAVE_CONFIG));
        string_type kwd(BOOST_WAVE_PRAGMA_KEYWORD);
        string_type strtype(BOOST_PP_STRINGIZE((BOOST_WAVE_STRINGTYPE)));
        ar & cfg;
        ar & kwd;
        ar & strtype;
        
        ar & language;
        ar & macros;
        ar & includes;
    }
    template<class Archive>
    void load(Archive & ar, const unsigned int loaded_version)
    {
        if (version != (loaded_version & ~version_mask)) {
            BOOST_WAVE_THROW(preprocess_exception, incompatible_config, 
                "cpp_context state version", get_main_pos());
        }
        
        // check compatibility of the stored information
        typedef typename token_type::string_type string_type;
        string_type config, pragma_keyword, string_type_str;
        
        ar & config;          // BOOST_WAVE_CONFIG
        if (config != BOOST_PP_STRINGIZE(BOOST_WAVE_CONFIG)) {
            BOOST_WAVE_THROW(preprocess_exception, incompatible_config, 
                "BOOST_WAVE_CONFIG", get_main_pos());
        }
        
        ar & pragma_keyword;  // BOOST_WAVE_PRAGMA_KEYWORD
        if (pragma_keyword != BOOST_WAVE_PRAGMA_KEYWORD) {
            BOOST_WAVE_THROW(preprocess_exception, incompatible_config, 
                "BOOST_WAVE_PRAGMA_KEYWORD", get_main_pos());
        }

        ar & string_type_str; // BOOST_PP_STRINGIZE((BOOST_WAVE_STRINGTYPE))
        if (string_type_str != BOOST_PP_STRINGIZE((BOOST_WAVE_STRINGTYPE))) {
            BOOST_WAVE_THROW(preprocess_exception, incompatible_config, 
                "BOOST_WAVE_STRINGTYPE", get_main_pos());
        }
        
        // read in the useful bits
        ar & language;
        ar & macros;
        ar & includes;
    }
    BOOST_SERIALIZATION_SPLIT_MEMBER()
#endif

private:
// the main input stream
    target_iterator_type first;         // underlying input stream
    target_iterator_type last;
    std::string filename;               // associated main filename
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
    std::string current_filename;       // real name of current preprocessed file
#endif 
    
    boost::wave::util::if_block_stack ifblocks;   // conditional compilation contexts
    boost::wave::util::include_paths includes;    // lists of include directories to search
    iteration_context_stack_type iter_ctxs;       // iteration contexts
    boost::wave::util::macromap<self_type> macros;  // map of defined macros
    boost::wave::language_support language;       // supported language/extensions
    hook_policy_type hooks;                       // hook policy instance
};

///////////////////////////////////////////////////////////////////////////////
}   // namespace wave
}   // namespace boost

#if BOOST_WAVE_SERIALIZATION != 0
namespace boost { namespace serialization {

template<
    typename Iterator, typename LexIterator, 
    typename InputPolicy, typename Hooks
>
struct tracking_level<boost::wave::context<Iterator, LexIterator, InputPolicy, Hooks> >
{
    typedef mpl::integral_c_tag tag;
    typedef mpl::int_<track_never> type;
    BOOST_STATIC_CONSTANT(
        int,
        value = tracking_level::type::value
    );
};

template<
    typename Iterator, typename LexIterator, 
    typename InputPolicy, typename Hooks
>
struct version<boost::wave::context<Iterator, LexIterator, InputPolicy, Hooks> >
{
    typedef boost::wave::context<Iterator, LexIterator, InputPolicy, Hooks>
        target_type;
    typedef mpl::int_<target_type::version> type;
    typedef mpl::integral_c_tag tag;
    BOOST_STATIC_CONSTANT(unsigned int, value = version::type::value);
};

}}  // namespace boost::serialization
#endif

// the suffix header occurs after all of the code
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_SUFFIX
#endif

#endif // !defined(CPP_CONTEXT_HPP_907485E2_6649_4A87_911B_7F7225F3E5B8_INCLUDED)
