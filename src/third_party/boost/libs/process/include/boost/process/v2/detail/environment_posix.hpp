//
// process/environment/detail/environment_posix.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2021 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_PROCESS_V2_DETAIL_ENVIRONMENT_POSIX_HPP
#define BOOST_PROCESS_V2_DETAIL_ENVIRONMENT_POSIX_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/cstring_ref.hpp>

#if defined(__APPLE__)
# include <crt_externs.h>
# if !defined(environ)
#  define environ (*_NSGetEnviron())
# endif
#elif defined(__MACH__) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__sun)
 extern "C" { extern char **environ; }
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace environment
{

using char_type = char;

template<typename Char>
using key_char_traits = std::char_traits<Char>;

template<typename Char>
using value_char_traits = std::char_traits<Char>;


constexpr char_type equality_sign = '=';
constexpr char_type delimiter = ':';

namespace detail
{

BOOST_PROCESS_V2_DECL
basic_cstring_ref<char_type, value_char_traits<char>>
get(basic_cstring_ref<char_type, key_char_traits<char_type>> key, error_code & ec);

BOOST_PROCESS_V2_DECL
void set(basic_cstring_ref<char_type, key_char_traits<char_type>> key,
         basic_cstring_ref<char_type, value_char_traits<char_type>> value,
         error_code & ec);

BOOST_PROCESS_V2_DECL
void unset(basic_cstring_ref<char_type, key_char_traits<char_type>> key,
           error_code & ec);
}


using native_handle_type   = const char * const *;
using native_iterator = native_handle_type;

namespace detail
{

BOOST_PROCESS_V2_DECL native_handle_type load_native_handle();
struct native_handle_deleter
{
    void operator()(native_handle_type) const {}
};

BOOST_PROCESS_V2_DECL native_iterator next(native_handle_type nh);
BOOST_PROCESS_V2_DECL native_iterator find_end(native_handle_type nh);
inline const char_type * dereference(native_iterator iterator) {return *iterator;}

BOOST_PROCESS_V2_DECL bool has_x_access(const char * pth);

inline bool is_executable(const filesystem::path & pth, error_code & ec)
{
  return filesystem::is_regular_file(pth, ec) && has_x_access(pth.c_str());
}

}

}

BOOST_PROCESS_V2_END_NAMESPACE

#endif
