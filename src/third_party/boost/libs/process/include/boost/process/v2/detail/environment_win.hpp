//
// process/environment/detail/environment_win.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2021 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_PROCESS_V2_DETAIL_ENVIRONMENT_WIN_HPP
#define BOOST_PROCESS_V2_DETAIL_ENVIRONMENT_WIN_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/cstring_ref.hpp>

#include <cwctype>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace environment
{

using char_type = wchar_t;

template<typename Char>
struct key_char_traits
{
  typedef Char      char_type;
  typedef typename std::char_traits<char_type>::int_type  int_type;
  typedef typename std::char_traits<char_type>::off_type   off_type;
  typedef typename std::char_traits<char_type>::pos_type   pos_type;
  typedef typename std::char_traits<char_type>::state_type state_type;

  BOOST_CONSTEXPR static char    to_lower(char c)    {return std::tolower(to_int_type(c));}
  BOOST_CONSTEXPR static wchar_t to_lower(wchar_t c) {return std::towlower(to_int_type(c));}

  BOOST_CONSTEXPR static int_type to_lower(int_type i, char )   {return std::tolower(i);}
  BOOST_CONSTEXPR static int_type to_lower(int_type i, wchar_t) {return std::towlower(i);}


  BOOST_CONSTEXPR static
  void assign(char_type& c1, const char_type& c2) BOOST_NOEXCEPT
  {
    c1 = c2;
  }

  BOOST_CONSTEXPR static
  bool eq(char_type c1, char_type c2) BOOST_NOEXCEPT
  {
    return to_lower(c1) == to_lower(c2);
  }

  BOOST_CONSTEXPR static
  bool lt(char_type c1, char_type c2) BOOST_NOEXCEPT
  {
    return to_lower(c1) < to_lower(c2);
  }

  BOOST_CXX14_CONSTEXPR static
  int compare(const char_type* s1, const char_type* s2, size_t n) BOOST_NOEXCEPT
  {
    auto itrs = std::mismatch(s1, s1 + n, s2, &eq);
    if (itrs.first == (s1 + n))
      return 0;
    auto c1 = to_lower(*itrs.first);
    auto c2 = to_lower(*itrs.second);

    return (c1 < c2 ) ? -1 : 1;
  }

  static size_t length(const char* s)    BOOST_NOEXCEPT  { return std::strlen(s); }
  static size_t length(const wchar_t* s) BOOST_NOEXCEPT  { return std::wcslen(s); }

  BOOST_CXX14_CONSTEXPR static
  const char_type* find(const char_type* s, size_t n, const char_type& a) BOOST_NOEXCEPT
  {
    const char_type u = to_lower(a);
    return std::find_if(s, s + n, [u](char_type c){return to_lower(c) == u;});
  }

  BOOST_CXX14_CONSTEXPR static
  char_type* move(char_type* s1, const char_type* s2, size_t n) BOOST_NOEXCEPT
  {
    if (s1 < s2)
      return std::move(s2, s2 + n, s1);
    else
      return std::move_backward(s2, s2 + n, s1 + n);
  }

  BOOST_CONSTEXPR static
  char_type* copy(char_type* s1, const char_type* s2, size_t n) BOOST_NOEXCEPT
  {
    return std::copy(s2, s2 + n, s1);
  }

  BOOST_CXX14_CONSTEXPR static
  char_type* assign(char_type* s, size_t n, char_type a) BOOST_NOEXCEPT
  {
    std::fill(s, s + n, a);
    return s +n;
  }

  BOOST_CONSTEXPR static
  int_type not_eof(int_type c) BOOST_NOEXCEPT
  {
    return eq_int_type(c, eof()) ? ~eof() : c;
  }

  BOOST_CONSTEXPR static
  char_type to_char_type(int_type c) BOOST_NOEXCEPT
  {
    return char_type(c);
  }

  BOOST_CONSTEXPR static
  int_type to_int_type(char c) BOOST_NOEXCEPT
  {
    return int_type((unsigned char)c);
  }

  BOOST_CONSTEXPR static
  int_type to_int_type(wchar_t c) BOOST_NOEXCEPT
  {
    return int_type((wchar_t)c);
  }

  BOOST_CONSTEXPR static
  bool eq_int_type(int_type c1, int_type c2) BOOST_NOEXCEPT
  {
    return to_lower(c1, char_type()) == to_lower(c2, char_type());
  }

  BOOST_CONSTEXPR static inline int_type eof() BOOST_NOEXCEPT
  {
    return int_type(EOF);
  }
};

namespace detail
{


template<typename Char>
std::size_t hash_step(std::size_t prev, Char c, key_char_traits<Char>)
{
    return prev ^ (key_char_traits<Char>::to_lower(c) << 1);
}


}

template<typename Char>
using value_char_traits = std::char_traits<Char>;

BOOST_CONSTEXPR static char_type equality_sign = L'=';
BOOST_CONSTEXPR static char_type delimiter = L';';

using native_handle_type   = wchar_t*;
using native_iterator = const wchar_t*;

namespace detail
{

BOOST_PROCESS_V2_DECL
std::basic_string<wchar_t, value_char_traits<wchar_t>> get(
        basic_cstring_ref<wchar_t, key_char_traits<wchar_t>> key,
        error_code & ec);

BOOST_PROCESS_V2_DECL
void set(basic_cstring_ref<wchar_t,   key_char_traits<wchar_t>> key,
         basic_cstring_ref<wchar_t, value_char_traits<wchar_t>> value,
         error_code & ec);

BOOST_PROCESS_V2_DECL
void unset(basic_cstring_ref<wchar_t, key_char_traits<wchar_t>> key,
           error_code & ec);


BOOST_PROCESS_V2_DECL
std::basic_string<char, value_char_traits<char>> get(
        basic_cstring_ref<char, key_char_traits<char>> key,
        error_code & ec);

BOOST_PROCESS_V2_DECL
void set(basic_cstring_ref<char,   key_char_traits<char>> key,
         basic_cstring_ref<char, value_char_traits<char>> value,
         error_code & ec);

BOOST_PROCESS_V2_DECL
void unset(basic_cstring_ref<char, key_char_traits<char>> key,
           error_code & ec);

BOOST_PROCESS_V2_DECL native_handle_type load_native_handle();
struct native_handle_deleter
{
  native_handle_deleter() = default;
  native_handle_deleter(const native_handle_deleter& ) = default;
  BOOST_PROCESS_V2_DECL void operator()(native_handle_type nh) const;

};

inline const char_type * dereference(native_iterator iterator) {return iterator;}
BOOST_PROCESS_V2_DECL native_iterator next(native_iterator nh);
BOOST_PROCESS_V2_DECL native_iterator find_end(native_handle_type nh);


BOOST_PROCESS_V2_DECL bool is_exec_type(const wchar_t * pth);

inline bool is_executable(const filesystem::path & pth, error_code & ec)
{
  return filesystem::is_regular_file(pth, ec) && is_exec_type(pth.c_str());
}

}

}
BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_DETAIL_ENVIRONMENT_WIN_HPP
