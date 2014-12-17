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
  *   FILE:        posix_api.cpp
  *   VERSION:     see <boost/version.hpp>
  *   DESCRIPTION: Implements the Posix API wrappers.
  */

#define BOOST_REGEX_SOURCE

#include <boost/config.hpp>
#include <cstdio>
#include <boost/regex.hpp>
#include <boost/cregex.hpp>

#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{
   using ::sprintf;
   using ::strcpy;
   using ::strcmp;
}
#endif


namespace boost{

namespace{

unsigned int magic_value = 25631;

const char* names[] = {
      "REG_NOERROR",
      "REG_NOMATCH",
      "REG_BADPAT",
      "REG_ECOLLATE",
      "REG_ECTYPE",
      "REG_EESCAPE",
      "REG_ESUBREG",
      "REG_EBRACK",
      "REG_EPAREN",
      "REG_EBRACE",
      "REG_BADBR",
      "REG_ERANGE",
      "REG_ESPACE",
      "REG_BADRPT",
      "REG_EEND",
      "REG_ESIZE",
      "REG_ERPAREN",
      "REG_EMPTY",
      "REG_ECOMPLEXITY",
      "REG_ESTACK",
      "REG_E_PERL",
      "REG_E_UNKNOWN",
};
} // namespace

typedef boost::basic_regex<char, c_regex_traits<char> > c_regex_type;

BOOST_REGEX_DECL int BOOST_REGEX_CCALL regcompA(regex_tA* expression, const char* ptr, int f)
{
   if(expression->re_magic != magic_value)
   {
      expression->guts = 0;
#ifndef BOOST_NO_EXCEPTIONS
      try{
#endif
      expression->guts = new c_regex_type();
#ifndef BOOST_NO_EXCEPTIONS
      } catch(...)
      {
         return REG_ESPACE;
      }
#else
      if(0 == expression->guts)
         return REG_E_MEMORY;
#endif
   }
   // set default flags:
   boost::uint_fast32_t flags = (f & REG_PERLEX) ? 0 : ((f & REG_EXTENDED) ? regex::extended : regex::basic);
   expression->eflags = (f & REG_NEWLINE) ? match_not_dot_newline : match_default;
   // and translate those that are actually set:

   if(f & REG_NOCOLLATE)
   {
      flags |= regex::nocollate;
#ifndef BOOST_REGEX_V3
      flags &= ~regex::collate;
#endif
   }

   if(f & REG_NOSUB)
   {
      //expression->eflags |= match_any;
      flags |= regex::nosubs;
   }

   if(f & REG_NOSPEC)
      flags |= regex::literal;
   if(f & REG_ICASE)
      flags |= regex::icase;
   if(f & REG_ESCAPE_IN_LISTS)
      flags &= ~regex::no_escape_in_lists;
   if(f & REG_NEWLINE_ALT)
      flags |= regex::newline_alt;

   const char* p2;
   if(f & REG_PEND)
      p2 = expression->re_endp;
   else p2 = ptr + std::strlen(ptr);

   int result;

#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
      expression->re_magic = magic_value;
      static_cast<c_regex_type*>(expression->guts)->set_expression(ptr, p2, flags);
      expression->re_nsub = static_cast<c_regex_type*>(expression->guts)->mark_count();
      result = static_cast<c_regex_type*>(expression->guts)->error_code();
#ifndef BOOST_NO_EXCEPTIONS
   } 
   catch(const boost::regex_error& be)
   {
      result = be.code();
   }
   catch(...)
   {
      result = REG_E_UNKNOWN;
   }
#endif
   if(result)
      regfreeA(expression);
   return result;

}

BOOST_REGEX_DECL regsize_t BOOST_REGEX_CCALL regerrorA(int code, const regex_tA* e, char* buf, regsize_t buf_size)
{
   std::size_t result = 0;
   if(code & REG_ITOA)
   {
      code &= ~REG_ITOA;
      if(code <= (int)REG_E_UNKNOWN)
      {
         result = std::strlen(names[code]) + 1;
         if(buf_size >= result)
            re_detail::strcpy_s(buf, buf_size, names[code]);
         return result;
      }
      return result;
   }
   if(code == REG_ATOI)
   {
      char localbuf[5];
      if(e == 0)
         return 0;
      for(int i = 0; i <= (int)REG_E_UNKNOWN; ++i)
      {
         if(std::strcmp(e->re_endp, names[i]) == 0)
         {
            //
            // We're converting an integer i to a string, and since i <= REG_E_UNKNOWN
            // a five character string is *always* large enough:
            //
#if BOOST_WORKAROUND(BOOST_MSVC, >= 1400) && !defined(_WIN32_WCE) && !defined(UNDER_CE)
            int r = (::sprintf_s)(localbuf, 5, "%d", i);
#else
            int r = (std::sprintf)(localbuf, "%d", i);
#endif
            if(r < 0)
               return 0; // sprintf failed
            if(std::strlen(localbuf) < buf_size)
               re_detail::strcpy_s(buf, buf_size, localbuf);
            return std::strlen(localbuf) + 1;
         }
      }
#if BOOST_WORKAROUND(BOOST_MSVC, >= 1400) && !defined(_WIN32_WCE) && !defined(UNDER_CE)
      (::sprintf_s)(localbuf, 5, "%d", 0);
#else
      (std::sprintf)(localbuf, "%d", 0);
#endif
      if(std::strlen(localbuf) < buf_size)
         re_detail::strcpy_s(buf, buf_size, localbuf);
      return std::strlen(localbuf) + 1;
   }
   if(code <= (int)REG_E_UNKNOWN)
   {
      std::string p;
      if((e) && (e->re_magic == magic_value))
         p = static_cast<c_regex_type*>(e->guts)->get_traits().error_string(static_cast< ::boost::regex_constants::error_type>(code));
      else
      {
         p = re_detail::get_default_error_string(static_cast< ::boost::regex_constants::error_type>(code));
      }
      std::size_t len = p.size();
      if(len < buf_size)
      {
         re_detail::strcpy_s(buf, buf_size, p.c_str());
      }
      return len + 1;
   }
   if(buf_size)
      *buf = 0;
   return 0;
}

BOOST_REGEX_DECL int BOOST_REGEX_CCALL regexecA(const regex_tA* expression, const char* buf, regsize_t n, regmatch_t* array, int eflags)
{
#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable:4267)
#endif
   bool result = false;
   match_flag_type flags = match_default | expression->eflags;
   const char* end;
   const char* start;
   cmatch m;
   
   if(eflags & REG_NOTBOL)
      flags |= match_not_bol;
   if(eflags & REG_NOTEOL)
      flags |= match_not_eol;
   if(eflags & REG_STARTEND)
   {
      start = buf + array[0].rm_so;
      end = buf + array[0].rm_eo;
   }
   else
   {
      start = buf;
      end = buf + std::strlen(buf);
   }

#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
   if(expression->re_magic == magic_value)
   {
      result = regex_search(start, end, m, *static_cast<c_regex_type*>(expression->guts), flags);
   }
   else
      return result;
#ifndef BOOST_NO_EXCEPTIONS
   } catch(...)
   {
      return REG_E_UNKNOWN;
   }
#endif

   if(result)
   {
      // extract what matched:
      std::size_t i;
      for(i = 0; (i < n) && (i < expression->re_nsub + 1); ++i)
      {
         array[i].rm_so = (m[i].matched == false) ? -1 : (m[i].first - buf);
         array[i].rm_eo = (m[i].matched == false) ? -1 : (m[i].second - buf);
      }
      // and set anything else to -1:
      for(i = expression->re_nsub + 1; i < n; ++i)
      {
         array[i].rm_so = -1;
         array[i].rm_eo = -1;
      }
      return 0;
   }
   return REG_NOMATCH;
#ifdef BOOST_MSVC
#pragma warning(pop)
#endif
}

BOOST_REGEX_DECL void BOOST_REGEX_CCALL regfreeA(regex_tA* expression)
{
   if(expression->re_magic == magic_value)
   {
      delete static_cast<c_regex_type*>(expression->guts);
   }
   expression->re_magic = 0;
}

} // namespace boost




