/*=============================================================================
    Boost.Wave: A Standard compliant C++ preprocessor library

    Definition of the abstract lexer interface
    
    http://www.boost.org/

    Copyright (c) 2001-2007 Hartmut Kaiser. Distributed under the Boost
    Software License, Version 1.0. (See accompanying file
    LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#if !defined(CPP_LEX_INTERFACE_HPP_E83F52A4_90AC_4FBE_A9A7_B65F7F94C497_INCLUDED)
#define CPP_LEX_INTERFACE_HPP_E83F52A4_90AC_4FBE_A9A7_B65F7F94C497_INCLUDED

#include <boost/wave/wave_config.hpp>
#include <boost/wave/util/file_position.hpp>
#include <boost/wave/language_support.hpp>

// this must occur after all of the includes and before any code appears
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_PREFIX
#endif

// suppress warnings about dependent classes not being exported from the dll
#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable : 4251 4231 4660)
#endif

///////////////////////////////////////////////////////////////////////////////
namespace boost {
namespace wave {
namespace cpplexer {

#if BOOST_WAVE_SEPARATE_LEXER_INSTANTIATION != 0
#define BOOST_WAVE_NEW_LEXER_DECL BOOST_WAVE_DECL
#else
#define BOOST_WAVE_NEW_LEXER_DECL
#endif 

///////////////////////////////////////////////////////////////////////////////
//  
//  new_lexer_gen: generates a new instance of the required C++ lexer
//
///////////////////////////////////////////////////////////////////////////////
template <typename TokenT> struct lex_input_interface; 

template <
    typename IteratorT, 
    typename PositionT = boost::wave::util::file_position_type
>
struct BOOST_WAVE_NEW_LEXER_DECL new_lexer_gen
{
//  The NewLexer function allows the opaque generation of a new lexer object.
//  It is coupled to the token type to allow to decouple the lexer/token 
//  configurations at compile time.
    static lex_input_interface<lex_token<PositionT> > *
    new_lexer(IteratorT const &first, IteratorT const &last, 
        PositionT const &pos, boost::wave::language_support language);
};

#undef BOOST_WAVE_NEW_LEXER_DECL

///////////////////////////////////////////////////////////////////////////////
//
//  The lex_input_interface decouples the lex_iterator_shim from the actual 
//  lexer. This is done to allow compile time reduction.
//  Thanks to JCAB for having this idea.
//
///////////////////////////////////////////////////////////////////////////////

template <typename TokenT>
struct lex_input_interface 
{
    typedef typename TokenT::position_type position_type;
    
    virtual TokenT get() = 0;
    virtual void set_position(position_type const &pos) = 0;
    virtual ~lex_input_interface() {}
    
#if BOOST_WAVE_SUPPORT_PRAGMA_ONCE != 0
    virtual bool has_include_guards(std::string& guard_name) const = 0;
#endif    

//  The new_lexer function allows the opaque generation of a new lexer object.
//  It is coupled to the token type to allow to distinguish different 
//  lexer/token configurations at compile time.
    template <typename IteratorT>
    static lex_input_interface *
    new_lexer(IteratorT const &first, IteratorT const &last, 
        position_type const &pos, boost::wave::language_support language)
    { 
        return new_lexer_gen<IteratorT, position_type>::new_lexer (first, last, 
            pos, language); 
    }
};

///////////////////////////////////////////////////////////////////////////////
}   // namespace cpplexer
}   // namespace wave
}   // namespace boost 

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

// the suffix header occurs after all of the code
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_SUFFIX
#endif

#endif // !defined(CPP_LEX_INTERFACE_HPP_E83F52A4_90AC_4FBE_A9A7_B65F7F94C497_INCLUDED)
