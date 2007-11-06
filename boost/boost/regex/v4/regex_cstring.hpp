/*
 *
 * Copyright (c) 1998-2002
 * John Maddock
 *
 * Use, modification and distribution are subject to the 
 * Boost Software License, Version 1.0. (See accompanying file 
 * LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 *
 */
 
 /*
  *   LOCATION:    see http://www.boost.org for most recent version.
  *   FILE         regex_cstring.hpp
  *   VERSION      see <boost/version.hpp>
  *   DESCRIPTION: This is an internal header file, do not include directly.
  *                String support and helper functions, for regular
  *                expression library.
  */

#ifndef BOOST_REGEX_CSTRING_HPP
#define BOOST_REGEX_CSTRING_HPP

#ifndef BOOST_REGEX_CONFIG_HPP
#include <boost/regex/config.hpp>
#endif

#include <cstring>

namespace boost{
   namespace re_detail{

//
// start by defining some template function aliases for C API functions:
//

template <class charT>
std::size_t BOOST_REGEX_CALL re_strlen(const charT *s)
{
   std::size_t len = 0;
   while(*s)
   {
      ++s;
      ++len;
   }
   return len;
}

inline std::size_t BOOST_REGEX_CALL re_strlen(const char *s)
{
   return std::strlen(s);
}

#ifndef BOOST_NO_WREGEX

inline std::size_t BOOST_REGEX_CALL re_strlen(const wchar_t *s)
{
   return std::wcslen(s);
}

#endif

#ifndef BOOST_NO_WREGEX
BOOST_REGEX_DECL void BOOST_REGEX_CALL re_transform(std::basic_string<wchar_t>& out, const std::basic_string<wchar_t>& in);
#endif
BOOST_REGEX_DECL void BOOST_REGEX_CALL re_transform(std::string& out, const std::string& in);

template <class charT>
void BOOST_REGEX_CALL re_trunc_primary(std::basic_string<charT>& s)
{
   for(unsigned int i = 0; i < s.size(); ++i)
   {
      if(s[i] <= 1)
      {
         s.erase(i);
         break;
      }
   }
}

inline char* BOOST_REGEX_CALL re_strcpy(char *s1, const char *s2)
{
   #if defined(__BORLANDC__) && defined(strcpy)
   return ::strcpy(s1, s2);
   #else
   return std::strcpy(s1, s2);
   #endif
}

#ifndef BOOST_NO_WREGEX

inline wchar_t* BOOST_REGEX_CALL re_strcpy(wchar_t *s1, const wchar_t *s2)
{
   return std::wcscpy(s1, s2);
}

#endif


template <class charT>
charT* BOOST_REGEX_CALL re_strdup(const charT* p)
{
   charT* buf = new charT[re_strlen(p) + 1];
   re_strcpy(buf, p);
   return buf;
}

template <class charT>
inline void BOOST_REGEX_CALL re_strfree(charT* p)
{
   delete[] p;
}

} // namespace re_detail
} // namespace boost

#endif  // BOOST_REGEX_CSTRING_HPP






