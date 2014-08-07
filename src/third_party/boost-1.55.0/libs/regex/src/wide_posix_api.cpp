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
  *   FILE:        wide_posix_api.cpp
  *   VERSION:     see <boost/version.hpp>
  *   DESCRIPTION: Implements the wide character POSIX API wrappers.
  */

#define BOOST_REGEX_SOURCE

#include <boost/regex/config.hpp>

#ifndef BOOST_NO_WREGEX

#include <boost/regex.hpp>
#include <boost/cregex.hpp>

#include <cwchar>
#include <cstring>
#include <cstdio>

#ifdef BOOST_INTEL
#pragma warning(disable:981)
#endif

#if defined(BOOST_NO_STDC_NAMESPACE) || defined(__NetBSD__)
namespace std{
#  ifndef BOOST_NO_SWPRINTF
      using ::swprintf;
#  endif
}
#endif


namespace boost{

namespace {

unsigned int wmagic_value = 28631;

const wchar_t* wnames[] = {
      L"REG_NOERROR",
      L"REG_NOMATCH",
      L"REG_BADPAT",
      L"REG_ECOLLATE",
      L"REG_ECTYPE",
      L"REG_EESCAPE",
      L"REG_ESUBREG",
      L"REG_EBRACK",
      L"REG_EPAREN",
      L"REG_EBRACE",
      L"REG_BADBR",
      L"REG_ERANGE",
      L"REG_ESPACE",
      L"REG_BADRPT",
      L"REG_EEND",
      L"REG_ESIZE",
      L"REG_ERPAREN",
      L"REG_EMPTY",
      L"REG_ECOMPLEXITY",
      L"REG_ESTACK",
      L"REG_E_PERL",
      L"REG_E_UNKNOWN",
};
}

typedef boost::basic_regex<wchar_t, c_regex_traits<wchar_t> > wc_regex_type;

BOOST_REGEX_DECL int BOOST_REGEX_CCALL regcompW(regex_tW* expression, const wchar_t* ptr, int f)
{
   if(expression->re_magic != wmagic_value)
   {
      expression->guts = 0;
#ifndef BOOST_NO_EXCEPTIONS
      try{
#endif
      expression->guts = new wc_regex_type();
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
   boost::uint_fast32_t flags = (f & REG_PERLEX) ? 0 : ((f & REG_EXTENDED) ? wregex::extended : wregex::basic);
   expression->eflags = (f & REG_NEWLINE) ? match_not_dot_newline : match_default;

   // and translate those that are actually set:
   if(f & REG_NOCOLLATE)
   {
      flags |= wregex::nocollate;
#ifndef BOOST_REGEX_V3
      flags &= ~wregex::collate;
#endif
   }

   if(f & REG_NOSUB)
   {
      //expression->eflags |= match_any;
      flags |= wregex::nosubs;
   }

   if(f & REG_NOSPEC)
      flags |= wregex::literal;
   if(f & REG_ICASE)
      flags |= wregex::icase;
   if(f & REG_ESCAPE_IN_LISTS)
      flags &= ~wregex::no_escape_in_lists;
   if(f & REG_NEWLINE_ALT)
      flags |= wregex::newline_alt;

   const wchar_t* p2;
   if(f & REG_PEND)
      p2 = expression->re_endp;
   else p2 = ptr + std::wcslen(ptr);

   int result;

#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
      expression->re_magic = wmagic_value;
      static_cast<wc_regex_type*>(expression->guts)->set_expression(ptr, p2, flags);
      expression->re_nsub = static_cast<wc_regex_type*>(expression->guts)->mark_count() - 1;
      result = static_cast<wc_regex_type*>(expression->guts)->error_code();
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
      regfreeW(expression);
   return result;

}

BOOST_REGEX_DECL regsize_t BOOST_REGEX_CCALL regerrorW(int code, const regex_tW* e, wchar_t* buf, regsize_t buf_size)
{
   std::size_t result = 0;
   if(code & REG_ITOA)
   {
      code &= ~REG_ITOA;
      if((code <= (int)REG_E_UNKNOWN) && (code >= 0))
      {
         result = std::wcslen(wnames[code]) + 1;
         if(buf_size >= result)
#if BOOST_WORKAROUND(BOOST_MSVC, >= 1400) && !defined(_WIN32_WCE) && !defined(UNDER_CE)
            ::wcscpy_s(buf, buf_size, wnames[code]);
#else
            std::wcscpy(buf, wnames[code]);
#endif
         return result;
      }
      return result;
   }
#if !defined(BOOST_NO_SWPRINTF)
   if(code == REG_ATOI)
   {
      wchar_t localbuf[5];
      if(e == 0)
         return 0;
      for(int i = 0; i <= (int)REG_E_UNKNOWN; ++i)
      {
         if(std::wcscmp(e->re_endp, wnames[i]) == 0)
         {
#if defined(_WIN32_WCE) && !defined(UNDER_CE)
            (std::swprintf)(localbuf, L"%d", i);
#else
            (std::swprintf)(localbuf, 5, L"%d", i);
#endif
            if(std::wcslen(localbuf) < buf_size)
#if BOOST_WORKAROUND(BOOST_MSVC, >= 1400) && !defined(_WIN32_WCE) && !defined(UNDER_CE)
               ::wcscpy_s(buf, buf_size, localbuf);
#else
               std::wcscpy(buf, localbuf);
#endif
            return std::wcslen(localbuf) + 1;
         }
      }
#if defined(_WIN32_WCE) && !defined(UNDER_CE)
      (std::swprintf)(localbuf, L"%d", 0);
#else
      (std::swprintf)(localbuf, 5, L"%d", 0);
#endif
      if(std::wcslen(localbuf) < buf_size)
#if BOOST_WORKAROUND(BOOST_MSVC, >= 1400) && !defined(_WIN32_WCE) && !defined(UNDER_CE)
         ::wcscpy_s(buf, buf_size, localbuf);
#else
         std::wcscpy(buf, localbuf);
#endif
      return std::wcslen(localbuf) + 1;
   }
#endif
   if(code <= (int)REG_E_UNKNOWN)
   {
      std::string p;
      if((e) && (e->re_magic == wmagic_value))
         p = static_cast<wc_regex_type*>(e->guts)->get_traits().error_string(static_cast< ::boost::regex_constants::error_type>(code));
      else
      {
         p = re_detail::get_default_error_string(static_cast< ::boost::regex_constants::error_type>(code));
      }
      std::size_t len = p.size();
      if(len < buf_size)
      {
         re_detail::copy(p.c_str(), p.c_str() + p.size() + 1, buf);
      }
      return len + 1;
   }
   if(buf_size)
      *buf = 0;
   return 0;
}

BOOST_REGEX_DECL int BOOST_REGEX_CCALL regexecW(const regex_tW* expression, const wchar_t* buf, regsize_t n, regmatch_t* array, int eflags)
{
#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable:4267)
#endif
   bool result = false;
   match_flag_type flags = match_default | expression->eflags;
   const wchar_t* end;
   const wchar_t* start;
   wcmatch m;
   
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
      end = buf + std::wcslen(buf);
   }

#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
   if(expression->re_magic == wmagic_value)
   {
      result = regex_search(start, end, m, *static_cast<wc_regex_type*>(expression->guts), flags);
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

BOOST_REGEX_DECL void BOOST_REGEX_CCALL regfreeW(regex_tW* expression)
{
   if(expression->re_magic == wmagic_value)
   {
      delete static_cast<wc_regex_type*>(expression->guts);
   }
   expression->re_magic = 0;
}

} // namespace boost;

#endif




