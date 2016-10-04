/*
 *
 * Copyright (c) 2004
 * John Maddock
 *
 * Use, modification and distribution are subject to the 
 * Boost Software License, Version 1.0. (See accompanying file 
 * LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 *
 */

 /*
  *   LOCATION:    see http://www.boost.org for most recent version.
  *   FILE:        c_regex_traits.cpp
  *   VERSION:     see <boost/version.hpp>
  *   DESCRIPTION: Implements out of line c_regex_traits<char> members
  */


#define BOOST_REGEX_SOURCE

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>
#include "internals.hpp"

#if !BOOST_WORKAROUND(__BORLANDC__, < 0x560)

#include <boost/regex/v4/c_regex_traits.hpp>
#include <boost/regex/v4/primary_transform.hpp>
#include <boost/regex/v4/regex_traits_defaults.hpp>

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std{
   using ::strxfrm; using ::isspace;
   using ::ispunct; using ::isalpha;
   using ::isalnum; using ::iscntrl;
   using ::isprint; using ::isupper;
   using ::islower; using ::isdigit;
   using ::isxdigit; using ::strtol;
}
#endif

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

namespace boost{

c_regex_traits<char>::string_type BOOST_REGEX_CALL c_regex_traits<char>::transform(const char* p1, const char* p2)
{ 
   std::string result(10, ' ');
   std::size_t s = result.size();
   std::size_t r;
   std::string src(p1, p2);
   while(s < (r = std::strxfrm(&*result.begin(), src.c_str(), s)))
   {
#if defined(_CPPLIB_VER)
      //
      // A bug in VC11 and 12 causes the program to hang if we pass a null-string
      // to std::strxfrm, but only for certain locales :-(
      // Probably effects Intel and Clang or any compiler using the VC std library (Dinkumware).
      //
      if(r == INT_MAX)
      {
         result.erase();
         result.insert(result.begin(), static_cast<char>(0));
         return result;
      }
#endif
      result.append(r - s + 3, ' ');
      s = result.size();
   }
   result.erase(r);
   return result; 
}

c_regex_traits<char>::string_type BOOST_REGEX_CALL c_regex_traits<char>::transform_primary(const char* p1, const char* p2)
{
   static char s_delim;
   static const int s_collate_type = ::boost::BOOST_REGEX_DETAIL_NS::find_sort_syntax(static_cast<c_regex_traits<char>*>(0), &s_delim);
   std::string result;
   //
   // What we do here depends upon the format of the sort key returned by
   // sort key returned by this->transform:
   //
   switch(s_collate_type)
   {
   case ::boost::BOOST_REGEX_DETAIL_NS::sort_C:
   case ::boost::BOOST_REGEX_DETAIL_NS::sort_unknown:
      // the best we can do is translate to lower case, then get a regular sort key:
      {
         result.assign(p1, p2);
         for(std::string::size_type i = 0; i < result.size(); ++i)
            result[i] = static_cast<char>((std::tolower)(static_cast<unsigned char>(result[i])));
         result = transform(&*result.begin(), &*result.begin() + result.size());
         break;
      }
   case ::boost::BOOST_REGEX_DETAIL_NS::sort_fixed:
      {
         // get a regular sort key, and then truncate it:
         result = transform(p1, p2);
         result.erase(s_delim);
         break;
      }
   case ::boost::BOOST_REGEX_DETAIL_NS::sort_delim:
         // get a regular sort key, and then truncate everything after the delim:
         result = transform(p1, p2);
         if(result.size() && (result[0] == s_delim))
            break;
         std::size_t i;
         for(i = 0; i < result.size(); ++i)
         {
            if(result[i] == s_delim)
               break;
         }
         result.erase(i);
         break;
   }
   if(result.empty())
      result = std::string(1, char(0));
   return result;
}

c_regex_traits<char>::char_class_type BOOST_REGEX_CALL c_regex_traits<char>::lookup_classname(const char* p1, const char* p2)
{
   static const char_class_type masks[] = 
   {
      0,
      char_class_alnum, 
      char_class_alpha,
      char_class_blank,
      char_class_cntrl,
      char_class_digit,
      char_class_digit,
      char_class_graph,
      char_class_horizontal,
      char_class_lower,
      char_class_lower,
      char_class_print,
      char_class_punct,
      char_class_space,
      char_class_space,
      char_class_upper,
      char_class_unicode,
      char_class_upper,
      char_class_vertical,
      char_class_alnum | char_class_word, 
      char_class_alnum | char_class_word, 
      char_class_xdigit,
   };

   int idx = ::boost::BOOST_REGEX_DETAIL_NS::get_default_class_id(p1, p2);
   if(idx < 0)
   {
      std::string s(p1, p2);
      for(std::string::size_type i = 0; i < s.size(); ++i)
         s[i] = static_cast<char>((std::tolower)(static_cast<unsigned char>(s[i])));
      idx = ::boost::BOOST_REGEX_DETAIL_NS::get_default_class_id(&*s.begin(), &*s.begin() + s.size());
   }
   BOOST_ASSERT(std::size_t(idx+1) < sizeof(masks) / sizeof(masks[0]));
   return masks[idx+1];
}

bool BOOST_REGEX_CALL c_regex_traits<char>::isctype(char c, char_class_type mask)
{
   return
      ((mask & char_class_space) && (std::isspace)(static_cast<unsigned char>(c)))
      || ((mask & char_class_print) && (std::isprint)(static_cast<unsigned char>(c)))
      || ((mask & char_class_cntrl) && (std::iscntrl)(static_cast<unsigned char>(c)))
      || ((mask & char_class_upper) && (std::isupper)(static_cast<unsigned char>(c)))
      || ((mask & char_class_lower) && (std::islower)(static_cast<unsigned char>(c)))
      || ((mask & char_class_alpha) && (std::isalpha)(static_cast<unsigned char>(c)))
      || ((mask & char_class_digit) && (std::isdigit)(static_cast<unsigned char>(c)))
      || ((mask & char_class_punct) && (std::ispunct)(static_cast<unsigned char>(c)))
      || ((mask & char_class_xdigit) && (std::isxdigit)(static_cast<unsigned char>(c)))
      || ((mask & char_class_blank) && (std::isspace)(static_cast<unsigned char>(c)) && !::boost::BOOST_REGEX_DETAIL_NS::is_separator(c))
      || ((mask & char_class_word) && (c == '_'))
      || ((mask & char_class_vertical) && (::boost::BOOST_REGEX_DETAIL_NS::is_separator(c) || (c == '\v')))
      || ((mask & char_class_horizontal) && (std::isspace)(static_cast<unsigned char>(c)) && !::boost::BOOST_REGEX_DETAIL_NS::is_separator(c) && (c != '\v'));
}

c_regex_traits<char>::string_type BOOST_REGEX_CALL c_regex_traits<char>::lookup_collatename(const char* p1, const char* p2)
{
   std::string s(p1, p2);
   s = ::boost::BOOST_REGEX_DETAIL_NS::lookup_default_collate_name(s);
   if(s.empty() && (p2-p1 == 1))
      s.append(1, *p1);
   return s;
}

int BOOST_REGEX_CALL c_regex_traits<char>::value(char c, int radix)
{
   char b[2] = { c, '\0', };
   char* ep;
   int result = std::strtol(b, &ep, radix);
   if(ep == b)
      return -1;
   return result;
}

}
#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

#endif
