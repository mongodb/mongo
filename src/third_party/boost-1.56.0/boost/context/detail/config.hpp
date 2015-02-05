
//          Copyright Oliver Kowalke 2009.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_CONTEXT_DETAIL_CONFIG_H
#define BOOST_CONTEXT_DETAIL_CONFIG_H

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

#ifdef BOOST_CONTEXT_DECL
# undef BOOST_CONTEXT_DECL
#endif

#if (defined(BOOST_ALL_DYN_LINK) || defined(BOOST_CONTEXT_DYN_LINK) ) && ! defined(BOOST_CONTEXT_STATIC_LINK)
# if defined(BOOST_CONTEXT_SOURCE)
#  define BOOST_CONTEXT_DECL BOOST_SYMBOL_EXPORT
#  define BOOST_CONTEXT_BUILD_DLL
# else
#  define BOOST_CONTEXT_DECL BOOST_SYMBOL_IMPORT
# endif
#endif

#if ! defined(BOOST_CONTEXT_DECL)
# define BOOST_CONTEXT_DECL
#endif

#if ! defined(BOOST_CONTEXT_SOURCE) && ! defined(BOOST_ALL_NO_LIB) && ! defined(BOOST_CONTEXT_NO_LIB)
# define BOOST_LIB_NAME boost_context
# if defined(BOOST_ALL_DYN_LINK) || defined(BOOST_CONTEXT_DYN_LINK)
#  define BOOST_DYN_LINK
# endif
# include <boost/config/auto_link.hpp>
#endif

#undef BOOST_CONTEXT_CALLDECL
#if (defined(i386) || defined(__i386__) || defined(__i386) \
    || defined(__i486__) || defined(__i586__) || defined(__i686__) \
    || defined(__X86__) || defined(_X86_) || defined(__THW_INTEL__) \
    || defined(__I86__) || defined(__INTEL__) || defined(__IA32__) \
    || defined(_M_IX86) || defined(_I86_)) && defined(BOOST_WINDOWS)
# define BOOST_CONTEXT_CALLDECL __cdecl
#else
# define BOOST_CONTEXT_CALLDECL
#endif

#endif // BOOST_CONTEXT_DETAIL_CONFIG_H
