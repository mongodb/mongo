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
  *   FILE         w32_regex_traits.cpp
  *   VERSION      see <boost/version.hpp>
  *   DESCRIPTION: Implements w32_regex_traits<char> (and associated helper classes).
  */

#define BOOST_REGEX_SOURCE
#include <boost/regex/config.hpp>

#if defined(_WIN32) && !defined(BOOST_REGEX_NO_W32)
#include <boost/regex/regex_traits.hpp>
#include <boost/regex/pattern_except.hpp>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#define NOGDI
#include <windows.h>

#if defined(_MSC_VER) && !defined(_WIN32_WCE) && !defined(UNDER_CE)
#pragma comment(lib, "user32.lib")
#endif

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std{
   using ::memset;
}
#endif

namespace boost{ namespace re_detail{

#ifdef BOOST_NO_ANSI_APIS
UINT get_code_page_for_locale_id(lcid_type idx)
{
   WCHAR code_page_string[7];
   if (::GetLocaleInfoW(idx, LOCALE_IDEFAULTANSICODEPAGE, code_page_string, 7) == 0)
       return 0;

   return static_cast<UINT>(_wtol(code_page_string));
}
#endif


void w32_regex_traits_char_layer<char>::init() 
{
   // we need to start by initialising our syntax map so we know which
   // character is used for which purpose:
   std::memset(m_char_map, 0, sizeof(m_char_map));
   cat_type cat;
   std::string cat_name(w32_regex_traits<char>::get_catalog_name());
   if(cat_name.size())
   {
      cat = ::boost::re_detail::w32_cat_open(cat_name);
      if(!cat)
      {
         std::string m("Unable to open message catalog: ");
         std::runtime_error err(m + cat_name);
         ::boost::re_detail::raise_runtime_error(err);
      }
   }
   //
   // if we have a valid catalog then load our messages:
   //
   if(cat)
   {
      for(regex_constants::syntax_type i = 1; i < regex_constants::syntax_max; ++i)
      {
         string_type mss = ::boost::re_detail::w32_cat_get(cat, this->m_locale, i, get_default_syntax(i));
         for(string_type::size_type j = 0; j < mss.size(); ++j)
         {
            m_char_map[static_cast<unsigned char>(mss[j])] = i;
         }
      }
   }
   else
   {
      for(regex_constants::syntax_type i = 1; i < regex_constants::syntax_max; ++i)
      {
         const char* ptr = get_default_syntax(i);
         while(ptr && *ptr)
         {
            m_char_map[static_cast<unsigned char>(*ptr)] = i;
            ++ptr;
         }
      }
   }
   //
   // finish off by calculating our escape types:
   //
   unsigned char i = 'A';
   do
   {
      if(m_char_map[i] == 0)
      {
         if(::boost::re_detail::w32_is(this->m_locale, 0x0002u, (char)i)) 
            m_char_map[i] = regex_constants::escape_type_class;
         else if(::boost::re_detail::w32_is(this->m_locale, 0x0001u, (char)i)) 
            m_char_map[i] = regex_constants::escape_type_not_class;
      }
   }while(0xFF != i++);

   //
   // fill in lower case map:
   //
   char char_map[1 << CHAR_BIT];
   for(int ii = 0; ii < (1 << CHAR_BIT); ++ii)
      char_map[ii] = static_cast<char>(ii);
#ifndef BOOST_NO_ANSI_APIS
   int r = ::LCMapStringA(this->m_locale, LCMAP_LOWERCASE, char_map, 1 << CHAR_BIT, this->m_lower_map, 1 << CHAR_BIT);
   BOOST_ASSERT(r != 0);
#else
   UINT code_page = get_code_page_for_locale_id(this->m_locale);
   BOOST_ASSERT(code_page != 0);

   WCHAR wide_char_map[1 << CHAR_BIT];
   int conv_r = ::MultiByteToWideChar(code_page, 0,  char_map, 1 << CHAR_BIT,  wide_char_map, 1 << CHAR_BIT);
   BOOST_ASSERT(conv_r != 0);

   WCHAR wide_lower_map[1 << CHAR_BIT];
   int r = ::LCMapStringW(this->m_locale, LCMAP_LOWERCASE,  wide_char_map, 1 << CHAR_BIT,  wide_lower_map, 1 << CHAR_BIT);
   BOOST_ASSERT(r != 0);

   conv_r = ::WideCharToMultiByte(code_page, 0,  wide_lower_map, r,  this->m_lower_map, 1 << CHAR_BIT,  NULL, NULL);
   BOOST_ASSERT(conv_r != 0);
#endif
   if(r < (1 << CHAR_BIT))
   {
      // if we have multibyte characters then not all may have been given
      // a lower case mapping:
      for(int jj = r; jj < (1 << CHAR_BIT); ++jj)
         this->m_lower_map[jj] = static_cast<char>(jj);
   }

#ifndef BOOST_NO_ANSI_APIS
   r = ::GetStringTypeExA(this->m_locale, CT_CTYPE1, char_map, 1 << CHAR_BIT, this->m_type_map);
#else
   r = ::GetStringTypeExW(this->m_locale, CT_CTYPE1, wide_char_map, 1 << CHAR_BIT, this->m_type_map);
#endif
   BOOST_ASSERT(0 != r);
}

BOOST_REGEX_DECL lcid_type BOOST_REGEX_CALL w32_get_default_locale()
{
   return ::GetUserDefaultLCID();
}

BOOST_REGEX_DECL bool BOOST_REGEX_CALL w32_is_lower(char c, lcid_type idx)
{
#ifndef BOOST_NO_ANSI_APIS
   WORD mask;
   if(::GetStringTypeExA(idx, CT_CTYPE1, &c, 1, &mask) && (mask & C1_LOWER))
      return true;
   return false;
#else
   UINT code_page = get_code_page_for_locale_id(idx);
   if (code_page == 0)
       return false;

   WCHAR wide_c;
   if (::MultiByteToWideChar(code_page, 0,  &c, 1,  &wide_c, 1) == 0)
       return false;

   WORD mask;
   if(::GetStringTypeExW(idx, CT_CTYPE1, &wide_c, 1, &mask) && (mask & C1_LOWER))
      return true;
   return false;
#endif
}

BOOST_REGEX_DECL bool BOOST_REGEX_CALL w32_is_lower(wchar_t c, lcid_type idx)
{
   WORD mask;
   if(::GetStringTypeExW(idx, CT_CTYPE1, &c, 1, &mask) && (mask & C1_LOWER))
      return true;
   return false;
}
#ifdef BOOST_REGEX_HAS_OTHER_WCHAR_T
BOOST_REGEX_DECL bool BOOST_REGEX_CALL w32_is_lower(unsigned short ca, lcid_type idx)
{
   WORD mask;
   wchar_t c = ca;
   if(::GetStringTypeExW(idx, CT_CTYPE1, &c, 1, &mask) && (mask & C1_LOWER))
      return true;
   return false;
}
#endif

BOOST_REGEX_DECL bool BOOST_REGEX_CALL w32_is_upper(char c, lcid_type idx)
{
#ifndef BOOST_NO_ANSI_APIS
   WORD mask;
   if(::GetStringTypeExA(idx, CT_CTYPE1, &c, 1, &mask) && (mask & C1_UPPER))
      return true;
   return false;
#else
   UINT code_page = get_code_page_for_locale_id(idx);
   if (code_page == 0)
       return false;

   WCHAR wide_c;
   if (::MultiByteToWideChar(code_page, 0,  &c, 1,  &wide_c, 1) == 0)
       return false;

   WORD mask;
   if(::GetStringTypeExW(idx, CT_CTYPE1, &wide_c, 1, &mask) && (mask & C1_UPPER))
      return true;
   return false;
#endif
}

BOOST_REGEX_DECL bool BOOST_REGEX_CALL w32_is_upper(wchar_t c, lcid_type idx)
{
   WORD mask;
   if(::GetStringTypeExW(idx, CT_CTYPE1, &c, 1, &mask) && (mask & C1_UPPER))
      return true;
   return false;
}
#ifdef BOOST_REGEX_HAS_OTHER_WCHAR_T
BOOST_REGEX_DECL bool BOOST_REGEX_CALL w32_is_upper(unsigned short ca, lcid_type idx)
{
   WORD mask;
   wchar_t c = ca;
   if(::GetStringTypeExW(idx, CT_CTYPE1, &c, 1, &mask) && (mask & C1_UPPER))
      return true;
   return false;
}
#endif

void free_module(void* mod)
{
   ::FreeLibrary(static_cast<HMODULE>(mod));
}

BOOST_REGEX_DECL cat_type BOOST_REGEX_CALL w32_cat_open(const std::string& name)
{
#ifndef BOOST_NO_ANSI_APIS
   cat_type result(::LoadLibraryA(name.c_str()), &free_module);
   return result;
#else
   LPWSTR wide_name = (LPWSTR)_alloca( (name.size() + 1) * sizeof(WCHAR) );
   if (::MultiByteToWideChar(CP_ACP, 0,  name.c_str(), name.size(),  wide_name, name.size() + 1) == 0)
       return cat_type();

   cat_type result(::LoadLibraryW(wide_name), &free_module);
   return result;
#endif
}

BOOST_REGEX_DECL std::string BOOST_REGEX_CALL w32_cat_get(const cat_type& cat, lcid_type, int i, const std::string& def)
{
#ifndef BOOST_NO_ANSI_APIS
   char buf[256];
   if(0 == ::LoadStringA(
      static_cast<HMODULE>(cat.get()),
      i,
      buf,
      256
   ))
   {
      return def;
   }
#else
   WCHAR wbuf[256];
   int r = ::LoadStringW(
      static_cast<HMODULE>(cat.get()),
      i,
      wbuf,
      256
   );
   if (r == 0)
      return def;


   int buf_size = 1 + ::WideCharToMultiByte(CP_ACP, 0,  wbuf, r,  NULL, 0,  NULL, NULL);
   LPSTR buf = (LPSTR)_alloca(buf_size);
   if (::WideCharToMultiByte(CP_ACP, 0,  wbuf, r,  buf, buf_size,  NULL, NULL) == 0)
      return def; // failed conversion.
#endif
   return std::string(buf);
}

#ifndef BOOST_NO_WREGEX
BOOST_REGEX_DECL std::wstring BOOST_REGEX_CALL w32_cat_get(const cat_type& cat, lcid_type, int i, const std::wstring& def)
{
   wchar_t buf[256];
   if(0 == ::LoadStringW(
      static_cast<HMODULE>(cat.get()),
      i,
      buf,
      256
   ))
   {
      return def;
   }
   return std::wstring(buf);
}
#ifdef BOOST_REGEX_HAS_OTHER_WCHAR_T
BOOST_REGEX_DECL std::basic_string<unsigned short> BOOST_REGEX_CALL w32_cat_get(const cat_type& cat, lcid_type, int i, const std::basic_string<unsigned short>& def)
{
   unsigned short buf[256];
   if(0 == ::LoadStringW(
      static_cast<HMODULE>(cat.get()),
      i,
      (LPWSTR)buf,
      256
   ))
   {
      return def;
   }
   return std::basic_string<unsigned short>(buf);
}
#endif
#endif
BOOST_REGEX_DECL std::string BOOST_REGEX_CALL w32_transform(lcid_type idx, const char* p1, const char* p2)
{
#ifndef BOOST_NO_ANSI_APIS
   int bytes = ::LCMapStringA(
      idx,       // locale identifier
      LCMAP_SORTKEY,  // mapping transformation type
      p1,  // source string
      static_cast<int>(p2 - p1),        // number of characters in source string
      0,  // destination buffer
      0        // size of destination buffer
      );
   if(!bytes)
      return std::string(p1, p2);
   std::string result(++bytes, '\0');
   bytes = ::LCMapStringA(
      idx,       // locale identifier
      LCMAP_SORTKEY,  // mapping transformation type
      p1,  // source string
      static_cast<int>(p2 - p1),        // number of characters in source string
      &*result.begin(),  // destination buffer
      bytes        // size of destination buffer
      );
#else
   UINT code_page = get_code_page_for_locale_id(idx);
   if(code_page == 0)
      return std::string(p1, p2);

   int src_len = static_cast<int>(p2 - p1);
   LPWSTR wide_p1 = (LPWSTR)_alloca( (src_len + 1) * 2 );
   if(::MultiByteToWideChar(code_page, 0,  p1, src_len,  wide_p1, src_len + 1) == 0)
      return std::string(p1, p2);

   int bytes = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_SORTKEY,  // mapping transformation type
      wide_p1,  // source string
      src_len,        // number of characters in source string
      0,  // destination buffer
      0        // size of destination buffer
      );
   if(!bytes)
      return std::string(p1, p2);
   std::string result(++bytes, '\0');
   bytes = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_SORTKEY,  // mapping transformation type
      wide_p1,  // source string
      src_len,        // number of characters in source string
      (LPWSTR)&*result.begin(),  // destination buffer
      bytes        // size of destination buffer
      );
#endif
   if(bytes > static_cast<int>(result.size()))
      return std::string(p1, p2);
   while(result.size() && result[result.size()-1] == '\0')
   {
      result.erase(result.size()-1);
   }
   return result;
}

#ifndef BOOST_NO_WREGEX
BOOST_REGEX_DECL std::wstring BOOST_REGEX_CALL w32_transform(lcid_type idx, const wchar_t* p1, const wchar_t* p2)
{
   int bytes = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_SORTKEY,  // mapping transformation type
      p1,  // source string
      static_cast<int>(p2 - p1),        // number of characters in source string
      0,  // destination buffer
      0        // size of destination buffer
      );
   if(!bytes)
      return std::wstring(p1, p2);
   std::string result(++bytes, '\0');
   bytes = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_SORTKEY,  // mapping transformation type
      p1,  // source string
      static_cast<int>(p2 - p1),        // number of characters in source string
      reinterpret_cast<wchar_t*>(&*result.begin()),  // destination buffer *of bytes*
      bytes        // size of destination buffer
      );
   if(bytes > static_cast<int>(result.size()))
      return std::wstring(p1, p2);
   while(result.size() && result[result.size()-1] == L'\0')
   {
      result.erase(result.size()-1);
   }
   std::wstring r2;
   for(std::string::size_type i = 0; i < result.size(); ++i)
      r2.append(1, static_cast<wchar_t>(static_cast<unsigned char>(result[i])));
   return r2;
}
#ifdef BOOST_REGEX_HAS_OTHER_WCHAR_T
BOOST_REGEX_DECL std::basic_string<unsigned short> BOOST_REGEX_CALL w32_transform(lcid_type idx, const unsigned short* p1, const unsigned short* p2)
{
   int bytes = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_SORTKEY,  // mapping transformation type
      (LPCWSTR)p1,  // source string
      static_cast<int>(p2 - p1),        // number of characters in source string
      0,  // destination buffer
      0        // size of destination buffer
      );
   if(!bytes)
      return std::basic_string<unsigned short>(p1, p2);
   std::string result(++bytes, '\0');
   bytes = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_SORTKEY,  // mapping transformation type
      (LPCWSTR)p1,  // source string
      static_cast<int>(p2 - p1),        // number of characters in source string
      reinterpret_cast<wchar_t*>(&*result.begin()),  // destination buffer *of bytes*
      bytes        // size of destination buffer
      );
   if(bytes > static_cast<int>(result.size()))
      return std::basic_string<unsigned short>(p1, p2);
   while(result.size() && result[result.size()-1] == L'\0')
   {
      result.erase(result.size()-1);
   }
   std::basic_string<unsigned short> r2;
   for(std::string::size_type i = 0; i < result.size(); ++i)
      r2.append(1, static_cast<unsigned short>(static_cast<unsigned char>(result[i])));
   return r2;
}
#endif
#endif
BOOST_REGEX_DECL char BOOST_REGEX_CALL w32_tolower(char c, lcid_type idx)
{
   char result[2];
#ifndef BOOST_NO_ANSI_APIS
   int b = ::LCMapStringA(
      idx,       // locale identifier
      LCMAP_LOWERCASE,  // mapping transformation type
      &c,  // source string
      1,        // number of characters in source string
      result,  // destination buffer
      1);        // size of destination buffer
   if(b == 0)
      return c;
#else
   UINT code_page = get_code_page_for_locale_id(idx);
   if (code_page == 0)
      return c;

   WCHAR wide_c;
   if (::MultiByteToWideChar(code_page, 0,  &c, 1,  &wide_c, 1) == 0)
      return c;

   WCHAR wide_result;
   int b = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_LOWERCASE,  // mapping transformation type
      &wide_c,  // source string
      1,        // number of characters in source string
      &wide_result,  // destination buffer
      1);        // size of destination buffer
   if(b == 0)
      return c;

   if (::WideCharToMultiByte(code_page, 0,  &wide_result, 1,  result, 2,  NULL, NULL) == 0)
       return c;  // No single byte lower case equivalent available
#endif
   return result[0];
}

#ifndef BOOST_NO_WREGEX
BOOST_REGEX_DECL wchar_t BOOST_REGEX_CALL w32_tolower(wchar_t c, lcid_type idx)
{
   wchar_t result[2];
   int b = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_LOWERCASE,  // mapping transformation type
      &c,  // source string
      1,        // number of characters in source string
      result,  // destination buffer
      1);        // size of destination buffer
   if(b == 0)
      return c;
   return result[0];
}
#ifdef BOOST_REGEX_HAS_OTHER_WCHAR_T
BOOST_REGEX_DECL unsigned short BOOST_REGEX_CALL w32_tolower(unsigned short c, lcid_type idx)
{
   wchar_t result[2];
   int b = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_LOWERCASE,  // mapping transformation type
      (wchar_t const*)&c,  // source string
      1,        // number of characters in source string
      result,  // destination buffer
      1);        // size of destination buffer
   if(b == 0)
      return c;
   return result[0];
}
#endif
#endif
BOOST_REGEX_DECL char BOOST_REGEX_CALL w32_toupper(char c, lcid_type idx)
{
   char result[2];
#ifndef BOOST_NO_ANSI_APIS
   int b = ::LCMapStringA(
      idx,       // locale identifier
      LCMAP_UPPERCASE,  // mapping transformation type
      &c,  // source string
      1,        // number of characters in source string
      result,  // destination buffer
      1);        // size of destination buffer
   if(b == 0)
      return c;
#else
   UINT code_page = get_code_page_for_locale_id(idx);
   if(code_page == 0)
       return c;

   WCHAR wide_c;
   if (::MultiByteToWideChar(code_page, 0,  &c, 1,  &wide_c, 1) == 0)
       return c;

   WCHAR wide_result;
   int b = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_UPPERCASE,  // mapping transformation type
      &wide_c,  // source string
      1,        // number of characters in source string
      &wide_result,  // destination buffer
      1);        // size of destination buffer
   if(b == 0)
      return c;

   if (::WideCharToMultiByte(code_page, 0,  &wide_result, 1,  result, 2,  NULL, NULL) == 0)
       return c;  // No single byte upper case equivalent available.
#endif
   return result[0];
}

#ifndef BOOST_NO_WREGEX
BOOST_REGEX_DECL wchar_t BOOST_REGEX_CALL w32_toupper(wchar_t c, lcid_type idx)
{
   wchar_t result[2];
   int b = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_UPPERCASE,  // mapping transformation type
      &c,  // source string
      1,        // number of characters in source string
      result,  // destination buffer
      1);        // size of destination buffer
   if(b == 0)
      return c;
   return result[0];
}
#ifdef BOOST_REGEX_HAS_OTHER_WCHAR_T
BOOST_REGEX_DECL unsigned short BOOST_REGEX_CALL w32_toupper(unsigned short c, lcid_type idx)
{
   wchar_t result[2];
   int b = ::LCMapStringW(
      idx,       // locale identifier
      LCMAP_UPPERCASE,  // mapping transformation type
      (wchar_t const*)&c,  // source string
      1,        // number of characters in source string
      result,  // destination buffer
      1);        // size of destination buffer
   if(b == 0)
      return c;
   return result[0];
}
#endif
#endif
BOOST_REGEX_DECL bool BOOST_REGEX_CALL w32_is(lcid_type idx, boost::uint32_t m, char c)
{
   WORD mask;
#ifndef BOOST_NO_ANSI_APIS
   if(::GetStringTypeExA(idx, CT_CTYPE1, &c, 1, &mask) && (mask & m & w32_regex_traits_implementation<char>::mask_base))
      return true;
#else
   UINT code_page = get_code_page_for_locale_id(idx);
   if(code_page == 0)
       return false;

   WCHAR wide_c;
   if (::MultiByteToWideChar(code_page, 0,  &c, 1,  &wide_c, 1) == 0)
       return false;

   if(::GetStringTypeExW(idx, CT_CTYPE1, &wide_c, 1, &mask) && (mask & m & w32_regex_traits_implementation<char>::mask_base))
      return true;
#endif
   if((m & w32_regex_traits_implementation<char>::mask_word) && (c == '_'))
      return true;
   return false;
}

#ifndef BOOST_NO_WREGEX
BOOST_REGEX_DECL bool BOOST_REGEX_CALL w32_is(lcid_type idx, boost::uint32_t m, wchar_t c)
{
   WORD mask;
   if(::GetStringTypeExW(idx, CT_CTYPE1, &c, 1, &mask) && (mask & m & w32_regex_traits_implementation<wchar_t>::mask_base))
      return true;
   if((m & w32_regex_traits_implementation<wchar_t>::mask_word) && (c == '_'))
      return true;
   if((m & w32_regex_traits_implementation<wchar_t>::mask_unicode) && (c > 0xff))
      return true;
   return false;
}
#ifdef BOOST_REGEX_HAS_OTHER_WCHAR_T
BOOST_REGEX_DECL bool BOOST_REGEX_CALL w32_is(lcid_type idx, boost::uint32_t m, unsigned short c)
{
   WORD mask;
   if(::GetStringTypeExW(idx, CT_CTYPE1, (wchar_t const*)&c, 1, &mask) && (mask & m & w32_regex_traits_implementation<wchar_t>::mask_base))
      return true;
   if((m & w32_regex_traits_implementation<wchar_t>::mask_word) && (c == '_'))
      return true;
   if((m & w32_regex_traits_implementation<wchar_t>::mask_unicode) && (c > 0xff))
      return true;
   return false;
}
#endif
#endif

} // re_detail
} // boost

#endif

