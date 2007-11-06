/*=============================================================================
    Boost.Wave: A Standard compliant C++ preprocessor library

    Global application configuration
    
    http://www.boost.org/

    Copyright (c) 2001-2007 Hartmut Kaiser. Distributed under the Boost
    Software License, Version 1.0. (See accompanying file
    LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#if !defined(WAVE_CONFIG_HPP_F143F90A_A63F_4B27_AC41_9CA4F14F538D_INCLUDED)
#define WAVE_CONFIG_HPP_F143F90A_A63F_4B27_AC41_9CA4F14F538D_INCLUDED

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/version.hpp>
#include <boost/spirit/version.hpp>
#include <boost/wave/wave_version.hpp>

///////////////////////////////////////////////////////////////////////////////
//  Define the maximal include nesting depth allowed. If this value isn't 
//  defined it defaults to 1024
//
//  To define a new initial include nesting define the following constant 
//  before including this file.
//
#if !defined(BOOST_WAVE_MAX_INCLUDE_LEVEL_DEPTH)
#define BOOST_WAVE_MAX_INCLUDE_LEVEL_DEPTH 1024
#endif

///////////////////////////////////////////////////////////////////////////////
//  Decide, whether to support variadics and placemarkers
//
//  To implement support variadics and placemarkers define the following to 
//  something not equal to zero.
//
#if !defined(BOOST_WAVE_SUPPORT_VARIADICS_PLACEMARKERS)
#define BOOST_WAVE_SUPPORT_VARIADICS_PLACEMARKERS 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Decide, whether to implement a #warning directive as an extension to the 
//  C++ Standard (same as #error, but emits a warning, not an error)
//
//  To disable the implementation of #warning directives, define the following 
//  constant as zero before including this file.
//
#if !defined(BOOST_WAVE_SUPPORT_WARNING_DIRECTIVE)
#define BOOST_WAVE_SUPPORT_WARNING_DIRECTIVE 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Decide, whether to implement #pragma once 
//
//  To disable the implementation of #pragma once, define the following 
//  constant as zero before including this file.
//
#if !defined(BOOST_WAVE_SUPPORT_PRAGMA_ONCE)
#define BOOST_WAVE_SUPPORT_PRAGMA_ONCE 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Decide, whether to implement #pragma message("") 
//
//  To disable the implementation of #pragma message(""), define the following 
//  constant as zero before including this file.
//
#if !defined(BOOST_WAVE_SUPPORT_PRAGMA_MESSAGE)
#define BOOST_WAVE_SUPPORT_PRAGMA_MESSAGE 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Decide, whether to implement #include_next
//  Please note, that this is an extension to the C++ Standard.
//
//  To disable the implementation of #include_next, define the following 
//  constant as zero before including this file.
//
#if !defined(BOOST_WAVE_SUPPORT_INCLUDE_NEXT)
#define BOOST_WAVE_SUPPORT_INCLUDE_NEXT 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Undefine the following, to enable some MS specific language extensions:
//  __int8, __int16, __int32, __int64, __based, __declspec, __cdecl, 
//  __fastcall, __stdcall, __try, __except, __finally, __leave, __inline,
//  __asm, #region, #endregion
//
//  Note: By default this is enabled for Windows based systems, otherwise it's 
//        disabled.
#if !defined(BOOST_WAVE_SUPPORT_MS_EXTENSIONS)
#if defined(BOOST_WINDOWS)
#define BOOST_WAVE_SUPPORT_MS_EXTENSIONS 1
#else
#define BOOST_WAVE_SUPPORT_MS_EXTENSIONS 0
#endif
#endif

///////////////////////////////////////////////////////////////////////////////
//  Allow the message body of the #error and #warning directives to be 
//  preprocessed before the diagnostic is issued.
//
//  To disable preprocessing of the body of #error and #warning directives, 
//  define the following constant as zero before including this file.
//
#if !defined(BOOST_WAVE_PREPROCESS_ERROR_MESSAGE_BODY)
#define BOOST_WAVE_PREPROCESS_ERROR_MESSAGE_BODY 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Allow the #pragma directives to be returned to the caller (optionally after 
//  preprocessing the body) 
//
//  To disable the library to emit unknown #pragma directives, define the 
//  following constant as zero before including this file.
//
#if !defined(BOOST_WAVE_EMIT_PRAGMA_DIRECTIVES)
#define BOOST_WAVE_EMIT_PRAGMA_DIRECTIVES 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Allow the body of a #pragma directive to be preprocessed before the 
//  directive is returned to the caller.
//
//  To disable the preprocessing of the body of #pragma directives, define the 
//  following constant as zero before including this file.
//
#if !defined(BOOST_WAVE_PREPROCESS_PRAGMA_BODY)
#define BOOST_WAVE_PREPROCESS_PRAGMA_BODY 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Allow to define macros with the command line syntax (-DMACRO(x)=definition)
//
//  To disable the the possibility to define macros based on the command line 
//  syntax, define the following constant as zero before including this file.
//
#if !defined(BOOST_WAVE_ENABLE_COMMANDLINE_MACROS)
#define BOOST_WAVE_ENABLE_COMMANDLINE_MACROS 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Define the pragma keyword to be used by the library to recognize its own
//  pragma's. If nothing else is defined, then 'wave' will be used as the 
//  default keyword, i.e. the library recognizes all 
//
//      #pragma wave option [(argument)]
//
//  and dispatches the handling to the interpret_pragma() preprocessing hook 
//  function (see file: preprocessing_hooks.hpp). The arguments part of the
//  pragma is optional.
//  The BOOST_WAVE_PRAGMA_KEYWORD constant needs to be defined to
//  resolve as a string literal value.
#if !defined(BOOST_WAVE_PRAGMA_KEYWORD)
#define BOOST_WAVE_PRAGMA_KEYWORD "wave"
#endif 

///////////////////////////////////////////////////////////////////////////////
//  Define the string type to be used to store the token values and the file 
//  names inside a file_position template class
//
#if !defined(BOOST_WAVE_STRINGTYPE)

#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300) || \
    BOOST_WORKAROUND(__MWERKS__, < 0x3200) || \
    (defined(__DECCXX) && defined(__alpha)) || \
    defined(BOOST_WAVE_STRINGTYPE_USE_STDSTRING)
    
// VC7 isn't able to compile the flex_string class, fall back to std::string 
// CW up to 8.3 chokes as well *sigh*
// Tru64/CXX has linker problems when using flex_string
#define BOOST_WAVE_STRINGTYPE std::string
#if !defined(BOOST_WAVE_STRINGTYPE_USE_STDSTRING)
#define BOOST_WAVE_STRINGTYPE_USE_STDSTRING 1
#endif

#else
// use the following, if you have a fast std::allocator<char>
#define BOOST_WAVE_STRINGTYPE boost::wave::util::flex_string< \
        char, std::char_traits<char>, std::allocator<char>, \
        boost::wave::util::CowString</*char, */\
            boost::wave::util::AllocatorStringStorage<char> \
        > \
    > \
    /**/
    
/* #define BOOST_WAVE_STRINGTYPE boost::wave::util::flex_string< \
        char, std::char_traits<char>, boost::fast_pool_allocator<char>, \
        boost::wave::util::CowString<char, \
            boost::wave::util::AllocatorStringStorage<char, \
              boost::fast_pool_allocator<char> \
            > \
        > \
    > \
*/    /**/
    
//  This include is needed for the flex_string class used in the 
//  BOOST_WAVE_STRINGTYPE above.
#include <boost/wave/util/flex_string.hpp>

//  This include is needed for the boost::fast_allocator class used in the 
//  BOOST_WAVE_STRINGTYPE above.
//  Configure Boost.Pool thread support (for now: no thread support at all)
//#define BOOST_NO_MT
//#include <boost/pool/pool_alloc.hpp>

// Use the following, if you want to incorporate Maxim Yegorushkin's
// const_string library (http://sourceforge.net/projects/conststring/), which
// may be even faster, than using the flex_string class from above
//#define BOOST_WAVE_STRINGTYPE boost::const_string<char>
//
//#include <boost/const_string/const_string.hpp>
//#include <boost/const_string/io.hpp>
//#include <boost/const_string/concatenation.hpp>

#endif // BOOST_WORKAROUND(_MSC_VER, <= 1300)
#endif

///////////////////////////////////////////////////////////////////////////////
//  The following definition forces the Spirit tree code to use list's instead
//  of vectors, which may be more efficient on some platforms
// #define BOOST_SPIRIT_USE_LIST_FOR_TREES

///////////////////////////////////////////////////////////////////////////////
//  The following definition forces the Spirit tree code to use boost pool 
//  allocators in stead of the std::allocator for the vector/list's.
// #define BOOST_SPIRIT_USE_BOOST_ALLOCATOR_FOR_TREES

///////////////////////////////////////////////////////////////////////////////
//  Uncomment the following, if you need debug output, the 
//  BOOST_SPIRIT_DEBUG_FLAGS_CPP constants below help to fine control the 
//  amount of the generated debug output.
//#define BOOST_SPIRIT_DEBUG

///////////////////////////////////////////////////////////////////////////////
//  Debug flags for the Wave library, possible flags specified below.
//
//  Note: These flags take effect only if the BOOST_SPIRIT_DEBUG constant
//        above is defined as well.
#define BOOST_SPIRIT_DEBUG_FLAGS_CPP_GRAMMAR            0x0001
#define BOOST_SPIRIT_DEBUG_FLAGS_TIME_CONVERSION        0x0002
#define BOOST_SPIRIT_DEBUG_FLAGS_CPP_EXPR_GRAMMAR       0x0004
#define BOOST_SPIRIT_DEBUG_FLAGS_INTLIT_GRAMMAR         0x0008
#define BOOST_SPIRIT_DEBUG_FLAGS_CHLIT_GRAMMAR          0x0010
#define BOOST_SPIRIT_DEBUG_FLAGS_DEFINED_GRAMMAR        0x0020
#define BOOST_SPIRIT_DEBUG_FLAGS_PREDEF_MACROS_GRAMMAR  0x0040

#if !defined(BOOST_SPIRIT_DEBUG_FLAGS_CPP)
#define BOOST_SPIRIT_DEBUG_FLAGS_CPP    0    // default is no debugging
#endif 

///////////////////////////////////////////////////////////////////////////////
//
//  For all recognized preprocessor statements the output parse trees 
//  formatted as xml are printed. The formatted parse trees are streamed to the 
//  std::ostream defined by the WAVE_DUMP_PARSE_TREE_OUT constant.
//
//  To enable the output of these parse trees, define the following constant 
//  as zero something not equal to zero before including this file. 
//
#if !defined(BOOST_WAVE_DUMP_PARSE_TREE)
#define BOOST_WAVE_DUMP_PARSE_TREE 0
#endif
#if BOOST_WAVE_DUMP_PARSE_TREE != 0 && !defined(BOOST_WAVE_DUMP_PARSE_TREE_OUT)
#define BOOST_WAVE_DUMP_PARSE_TREE_OUT std::cerr
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  For all #if and #elif directives the preprocessed expressions are printed.
//  These expressions are streamed to the std::ostream defined by the 
//  BOOST_WAVE_DUMP_CONDITIONAL_EXPRESSIONS_OUT constant.
//
//  To enable the output of the preprocessed expressions, define the following 
//  constant as something not equal to zero before including this file.
//
#if !defined(BOOST_WAVE_DUMP_CONDITIONAL_EXPRESSIONS)
#define BOOST_WAVE_DUMP_CONDITIONAL_EXPRESSIONS 0
#endif
#if BOOST_WAVE_DUMP_CONDITIONAL_EXPRESSIONS != 0 && \
   !defined(BOOST_WAVE_DUMP_CONDITIONAL_EXPRESSIONS_OUT)
#define BOOST_WAVE_DUMP_CONDITIONAL_EXPRESSIONS_OUT std::cerr
#endif

///////////////////////////////////////////////////////////////////////////////
//  Decide, whether to use the separate compilation model for the instantiation 
//  of the C++ lexer objects.
//
//  If this is defined, you should explicitly instantiate the C++ lexer
//  template with the correct parameters in a separate compilation unit of
//  your program (see the file instantiate_re2c_lexer.cpp). 
//
//  To use the lexer inclusion model, define the following constant as 
//  something not equal to zero before including this file.
//
#if !defined(BOOST_WAVE_SEPARATE_LEXER_INSTANTIATION)
#define BOOST_WAVE_SEPARATE_LEXER_INSTANTIATION 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Decide, whether to use the separate compilation model for the instantiation 
//  of the grammar objects.
//
//  If this is defined, you should explicitly instantiate the grammar
//  templates with the correct parameters in a separate compilation unit of
//  your program (see the files instantiate_cpp_grammar.cpp et.al.). 
//
//  To use the grammar inclusion model, define the following constant as 
//  something not equal to zero before including this file.
//
#if !defined(BOOST_WAVE_SEPARATE_GRAMMAR_INSTANTIATION)
#define BOOST_WAVE_SEPARATE_GRAMMAR_INSTANTIATION 1
#endif

///////////////////////////////////////////////////////////////////////////////
//  Decide whether to use a strict C++ lexer.
//  
//  If this is defined to something != 0, then the C++ lexers recognize the 
//  strict C99/C++ basic source character set. If it is not defined or defined 
//  to zero, the C++ lexers recognize the '$' character as part of identifiers.
//
//  The default is to recognize the '$' character as part of identifiers.
//
#if !defined(BOOST_WAVE_USE_STRICT_LEXER)
#define BOOST_WAVE_USE_STRICT_LEXER 0
#endif

///////////////////////////////////////////////////////////////////////////////
//  Decide, whether the serialization of the wave::context class should be 
//  supported
//
//  If this is defined to something not equal to zero the generated code will
//  expose routines allowing to store and reload the internal state of the 
//  wave::context object.
//
//  To enable the availability of the serialization functionality, define the 
//  following constant as something not equal to zero before including this file.
//
#if !defined(BOOST_WAVE_SERIALIZATION)
#define BOOST_WAVE_SERIALIZATION 0
#endif

///////////////////////////////////////////////////////////////////////////////
//  configure Boost.Pool thread support (for now: no thread support at all)
#if !defined(BOOST_NO_MT)
#define BOOST_NO_MT
#endif // !defined(BOOST_NO_MT)

//#if !defined(BOOST_DISABLE_THREADS)
//#define BOOST_DISABLE_THREADS
//#endif // !defined(BOOST_DISABLE_THREADS)

///////////////////////////////////////////////////////////////////////////////
//  Wave needs at least 4 parameters for phoenix actors
#if !defined(PHOENIX_LIMIT)
#define PHOENIX_LIMIT 6
#endif
#if PHOENIX_LIMIT < 6
#error "Boost.Wave: the constant PHOENIX_LIMIT must be at least defined to 4" \
       " to compile the library."
#endif

///////////////////////////////////////////////////////////////////////////////
//  Set up dll import/export options
#if defined(BOOST_HAS_DECLSPEC) && \
    (defined(BOOST_WAVE_DYN_LINK) || defined(BOOST_ALL_DYN_LINK)) && \
    !defined(BOOST_WAVE_STATIC_LINK)
    
#if defined(BOOST_WAVE_SOURCE)
#define BOOST_WAVE_DECL __declspec(dllexport)
#define BOOST_WAVE_BUILD_DLL
#else
#define BOOST_WAVE_DECL __declspec(dllimport)
#endif

#endif // building a shared library

#ifndef BOOST_WAVE_DECL
#define BOOST_WAVE_DECL
#endif

///////////////////////////////////////////////////////////////////////////////
//  Auto library naming
#if BOOST_VERSION >= 103100   
// auto link features work beginning from Boost V1.31.0
#if !defined(BOOST_WAVE_SOURCE) && !defined(BOOST_ALL_NO_LIB) && \
    !defined(BOOST_WAVE_NO_LIB)

#define BOOST_LIB_NAME boost_wave

// tell the auto-link code to select a dll when required:
#if defined(BOOST_ALL_DYN_LINK) || defined(BOOST_WAVE_DYN_LINK)
#define BOOST_DYN_LINK
#endif

#include <boost/config/auto_link.hpp>

#endif  // auto-linking disabled
#endif  // BOOST_VERSION

#endif // !defined(WAVE_CONFIG_HPP_F143F90A_A63F_4B27_AC41_9CA4F14F538D_INCLUDED)
