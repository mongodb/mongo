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
  *   FILE         regex_kmp.hpp
  *   VERSION      see <boost/version.hpp>
  *   DESCRIPTION: Provides Knuth Morris Pratt search operations.
  *                Note this is an internal header file included
  *                by regex.hpp, do not include on its own.
  */

#ifndef BOOST_REGEX_KMP_HPP
#define BOOST_REGEX_KMP_HPP

#ifdef BOOST_REGEX_CONFIG_HPP
#include <boost/regex/config.hpp>
#endif


namespace boost{
   namespace re_detail{

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

template <class charT>
struct kmp_info
{
   unsigned int size;
   unsigned int len;
   const charT* pstr;
   int kmp_next[1];
};

template <class charT, class Allocator>
void kmp_free(kmp_info<charT>* pinfo, const Allocator& a)
{
   typedef typename boost::detail::rebind_allocator<char, Allocator>::type atype;
   atype(a).deallocate(reinterpret_cast<char*>(pinfo), pinfo->size);
}

template <class iterator, class charT, class Trans, class Allocator>
kmp_info<charT>* kmp_compile(iterator first, iterator last, charT, Trans translate, const Allocator& a) 
{    
   typedef typename boost::detail::rebind_allocator<char, Allocator>::type atype;
   int i, j, m;
   i = 0;
   m = static_cast<int>(::boost::re_detail::distance(first, last));
   ++m;
   unsigned int size = sizeof(kmp_info<charT>) + sizeof(int)*m + sizeof(charT)*m;
   --m;
   //
   // allocate struct and fill it in:
   //
   kmp_info<charT>* pinfo = reinterpret_cast<kmp_info<charT>*>(atype(a).allocate(size));
   BOOST_REGEX_NOEH_ASSERT(pinfo)
   pinfo->size = size;
   pinfo->len = m;
   charT* p = reinterpret_cast<charT*>(reinterpret_cast<char*>(pinfo) + sizeof(kmp_info<charT>) + sizeof(int)*(m+1));
   pinfo->pstr = p;
   while(first != last)
   {
      *p = translate(*first);
      ++first;
      ++p;
   }
   *p = 0;
   //
   // finally do regular kmp compile:
   //
   j = pinfo->kmp_next[0] = -1;
   while (i < m) 
   {
      while ((j > -1) && (pinfo->pstr[i] != pinfo->pstr[j])) 
         j = pinfo->kmp_next[j];
      ++i;
      ++j;
      if (pinfo->pstr[i] == pinfo->pstr[j]) 
         pinfo->kmp_next[i] = pinfo->kmp_next[j];
      else 
         pinfo->kmp_next[i] = j;
   }

   return pinfo;
}

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

   } // namepsace re_detail
} // namespace boost

#endif   // BOOST_REGEX_KMP_HPP




