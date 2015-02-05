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
  *   FILE         cpp_regex_traits.cpp
  *   VERSION      see <boost/version.hpp>
  *   DESCRIPTION: Implements cpp_regex_traits<char> (and associated helper classes).
  */

#define BOOST_REGEX_SOURCE
#include <boost/config.hpp>
#ifndef BOOST_NO_STD_LOCALE
#include <boost/regex/regex_traits.hpp>
#include <boost/regex/pattern_except.hpp>

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std{
   using ::memset;
}
#endif

namespace boost{ namespace re_detail{

void cpp_regex_traits_char_layer<char>::init() 
{
   // we need to start by initialising our syntax map so we know which
   // character is used for which purpose:
   std::memset(m_char_map, 0, sizeof(m_char_map));
#ifndef BOOST_NO_STD_MESSAGES
#ifndef __IBMCPP__
   std::messages<char>::catalog cat = static_cast<std::messages<char>::catalog>(-1);
#else
   std::messages<char>::catalog cat = reinterpret_cast<std::messages<char>::catalog>(-1);
#endif
   std::string cat_name(cpp_regex_traits<char>::get_catalog_name());
   if(cat_name.size() && (m_pmessages != 0))
   {
      cat = this->m_pmessages->open(
         cat_name, 
         this->m_locale);
      if((int)cat < 0)
      {
         std::string m("Unable to open message catalog: ");
         std::runtime_error err(m + cat_name);
         boost::re_detail::raise_runtime_error(err);
      }
   }
   //
   // if we have a valid catalog then load our messages:
   //
   if((int)cat >= 0)
   {
#ifndef BOOST_NO_EXCEPTIONS
      try{
#endif
         for(regex_constants::syntax_type i = 1; i < regex_constants::syntax_max; ++i)
         {
            string_type mss = this->m_pmessages->get(cat, 0, i, get_default_syntax(i));
            for(string_type::size_type j = 0; j < mss.size(); ++j)
            {
               m_char_map[static_cast<unsigned char>(mss[j])] = i;
            }
         }
         this->m_pmessages->close(cat);
#ifndef BOOST_NO_EXCEPTIONS
      }
      catch(...)
      {
         this->m_pmessages->close(cat);
         throw;
      }
#endif
   }
   else
   {
#endif
      for(regex_constants::syntax_type j = 1; j < regex_constants::syntax_max; ++j)
      {
         const char* ptr = get_default_syntax(j);
         while(ptr && *ptr)
         {
            m_char_map[static_cast<unsigned char>(*ptr)] = j;
            ++ptr;
         }
      }
#ifndef BOOST_NO_STD_MESSAGES
   }
#endif
   //
   // finish off by calculating our escape types:
   //
   unsigned char i = 'A';
   do
   {
      if(m_char_map[i] == 0)
      {
         if(this->m_pctype->is(std::ctype_base::lower, i)) 
            m_char_map[i] = regex_constants::escape_type_class;
         else if(this->m_pctype->is(std::ctype_base::upper, i)) 
            m_char_map[i] = regex_constants::escape_type_not_class;
      }
   }while(0xFF != i++);
}

} // re_detail
} // boost
#endif

