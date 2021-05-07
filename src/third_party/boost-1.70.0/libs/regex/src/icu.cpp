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
  *   FILE         icu.cpp
  *   VERSION      see <boost/version.hpp>
  *   DESCRIPTION: Unicode regular expressions on top of the ICU Library.
  */
#define BOOST_REGEX_SOURCE

#include <boost/regex/config.hpp>
#ifdef BOOST_HAS_ICU
#define BOOST_REGEX_ICU_INSTANTIATE
#include <boost/regex/icu.hpp>

#ifdef BOOST_INTEL
#pragma warning(disable:981 2259 383)
#endif

namespace boost{

namespace BOOST_REGEX_DETAIL_NS{

icu_regex_traits_implementation::string_type icu_regex_traits_implementation::do_transform(const char_type* p1, const char_type* p2, const U_NAMESPACE_QUALIFIER Collator* pcoll) const
{
   // TODO make thread safe!!!! :
   typedef u32_to_u16_iterator<const char_type*, ::UChar> itt;
   itt i(p1), j(p2);
#ifndef BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS
   std::vector< ::UChar> t(i, j);
#else
   std::vector< ::UChar> t;
   while(i != j)
      t.push_back(*i++);
#endif
   ::uint8_t result[100];
   ::int32_t len;
   if(t.size())
      len = pcoll->getSortKey(&*t.begin(), static_cast< ::int32_t>(t.size()), result, sizeof(result));
   else
      len = pcoll->getSortKey(static_cast<UChar const*>(0), static_cast< ::int32_t>(0), result, sizeof(result));
   if(std::size_t(len) > sizeof(result))
   {
      scoped_array< ::uint8_t> presult(new ::uint8_t[len+1]);
      if(t.size())
         len = pcoll->getSortKey(&*t.begin(), static_cast< ::int32_t>(t.size()), presult.get(), len+1);
      else
         len = pcoll->getSortKey(static_cast<UChar const*>(0), static_cast< ::int32_t>(0), presult.get(), len+1);
      if((0 == presult[len-1]) && (len > 1))
         --len;
#ifndef BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS
      return string_type(presult.get(), presult.get()+len);
#else
      string_type sresult;
      ::uint8_t const* ia = presult.get();
      ::uint8_t const* ib = presult.get()+len;
      while(ia != ib)
         sresult.push_back(*ia++);
      return sresult;
#endif
   }
   if((0 == result[len-1]) && (len > 1))
      --len;
#ifndef BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS
   return string_type(result, result+len);
#else
   string_type sresult;
   ::uint8_t const* ia = result;
   ::uint8_t const* ib = result+len;
   while(ia != ib)
      sresult.push_back(*ia++);
   return sresult;
#endif
}

}

icu_regex_traits::size_type icu_regex_traits::length(const char_type* p)
{
   size_type result = 0;
   while(*p)
   {
      ++p;
      ++result;
   }
   return result;
}

//
// define our bitmasks:
//
const icu_regex_traits::char_class_type icu_regex_traits::mask_blank = icu_regex_traits::char_class_type(1) << offset_blank;
const icu_regex_traits::char_class_type icu_regex_traits::mask_space = icu_regex_traits::char_class_type(1) << offset_space;
const icu_regex_traits::char_class_type icu_regex_traits::mask_xdigit = icu_regex_traits::char_class_type(1) << offset_xdigit;
const icu_regex_traits::char_class_type icu_regex_traits::mask_underscore = icu_regex_traits::char_class_type(1) << offset_underscore;
const icu_regex_traits::char_class_type icu_regex_traits::mask_unicode = icu_regex_traits::char_class_type(1) << offset_unicode;
const icu_regex_traits::char_class_type icu_regex_traits::mask_any = icu_regex_traits::char_class_type(1) << offset_any;
const icu_regex_traits::char_class_type icu_regex_traits::mask_ascii = icu_regex_traits::char_class_type(1) << offset_ascii;
const icu_regex_traits::char_class_type icu_regex_traits::mask_horizontal = icu_regex_traits::char_class_type(1) << offset_horizontal;
const icu_regex_traits::char_class_type icu_regex_traits::mask_vertical = icu_regex_traits::char_class_type(1) << offset_vertical;

icu_regex_traits::char_class_type icu_regex_traits::lookup_icu_mask(const ::UChar32* p1, const ::UChar32* p2)
{
   static const ::UChar32 prop_name_table[] = {
      /* any */  'a', 'n', 'y', 
      /* ascii */  'a', 's', 'c', 'i', 'i', 
      /* assigned */  'a', 's', 's', 'i', 'g', 'n', 'e', 'd', 
      /* c* */  'c', '*', 
      /* cc */  'c', 'c', 
      /* cf */  'c', 'f', 
      /* closepunctuation */  'c', 'l', 'o', 's', 'e', 'p', 'u', 'n', 'c', 't', 'u', 'a', 't', 'i', 'o', 'n', 
      /* cn */  'c', 'n', 
      /* co */  'c', 'o', 
      /* connectorpunctuation */  'c', 'o', 'n', 'n', 'e', 'c', 't', 'o', 'r', 'p', 'u', 'n', 'c', 't', 'u', 'a', 't', 'i', 'o', 'n', 
      /* control */  'c', 'o', 'n', 't', 'r', 'o', 'l', 
      /* cs */  'c', 's', 
      /* currencysymbol */  'c', 'u', 'r', 'r', 'e', 'n', 'c', 'y', 's', 'y', 'm', 'b', 'o', 'l', 
      /* dashpunctuation */  'd', 'a', 's', 'h', 'p', 'u', 'n', 'c', 't', 'u', 'a', 't', 'i', 'o', 'n', 
      /* decimaldigitnumber */  'd', 'e', 'c', 'i', 'm', 'a', 'l', 'd', 'i', 'g', 'i', 't', 'n', 'u', 'm', 'b', 'e', 'r', 
      /* enclosingmark */  'e', 'n', 'c', 'l', 'o', 's', 'i', 'n', 'g', 'm', 'a', 'r', 'k', 
      /* finalpunctuation */  'f', 'i', 'n', 'a', 'l', 'p', 'u', 'n', 'c', 't', 'u', 'a', 't', 'i', 'o', 'n', 
      /* format */  'f', 'o', 'r', 'm', 'a', 't', 
      /* initialpunctuation */  'i', 'n', 'i', 't', 'i', 'a', 'l', 'p', 'u', 'n', 'c', 't', 'u', 'a', 't', 'i', 'o', 'n', 
      /* l* */  'l', '*', 
      /* letter */  'l', 'e', 't', 't', 'e', 'r', 
      /* letternumber */  'l', 'e', 't', 't', 'e', 'r', 'n', 'u', 'm', 'b', 'e', 'r', 
      /* lineseparator */  'l', 'i', 'n', 'e', 's', 'e', 'p', 'a', 'r', 'a', 't', 'o', 'r', 
      /* ll */  'l', 'l', 
      /* lm */  'l', 'm', 
      /* lo */  'l', 'o', 
      /* lowercaseletter */  'l', 'o', 'w', 'e', 'r', 'c', 'a', 's', 'e', 'l', 'e', 't', 't', 'e', 'r', 
      /* lt */  'l', 't', 
      /* lu */  'l', 'u', 
      /* m* */  'm', '*', 
      /* mark */  'm', 'a', 'r', 'k', 
      /* mathsymbol */  'm', 'a', 't', 'h', 's', 'y', 'm', 'b', 'o', 'l', 
      /* mc */  'm', 'c', 
      /* me */  'm', 'e', 
      /* mn */  'm', 'n', 
      /* modifierletter */  'm', 'o', 'd', 'i', 'f', 'i', 'e', 'r', 'l', 'e', 't', 't', 'e', 'r', 
      /* modifiersymbol */  'm', 'o', 'd', 'i', 'f', 'i', 'e', 'r', 's', 'y', 'm', 'b', 'o', 'l', 
      /* n* */  'n', '*', 
      /* nd */  'n', 'd', 
      /* nl */  'n', 'l', 
      /* no */  'n', 'o', 
      /* nonspacingmark */  'n', 'o', 'n', 's', 'p', 'a', 'c', 'i', 'n', 'g', 'm', 'a', 'r', 'k', 
      /* notassigned */  'n', 'o', 't', 'a', 's', 's', 'i', 'g', 'n', 'e', 'd', 
      /* number */  'n', 'u', 'm', 'b', 'e', 'r', 
      /* openpunctuation */  'o', 'p', 'e', 'n', 'p', 'u', 'n', 'c', 't', 'u', 'a', 't', 'i', 'o', 'n', 
      /* other */  'o', 't', 'h', 'e', 'r', 
      /* otherletter */  'o', 't', 'h', 'e', 'r', 'l', 'e', 't', 't', 'e', 'r', 
      /* othernumber */  'o', 't', 'h', 'e', 'r', 'n', 'u', 'm', 'b', 'e', 'r', 
      /* otherpunctuation */  'o', 't', 'h', 'e', 'r', 'p', 'u', 'n', 'c', 't', 'u', 'a', 't', 'i', 'o', 'n', 
      /* othersymbol */  'o', 't', 'h', 'e', 'r', 's', 'y', 'm', 'b', 'o', 'l', 
      /* p* */  'p', '*', 
      /* paragraphseparator */  'p', 'a', 'r', 'a', 'g', 'r', 'a', 'p', 'h', 's', 'e', 'p', 'a', 'r', 'a', 't', 'o', 'r', 
      /* pc */  'p', 'c', 
      /* pd */  'p', 'd', 
      /* pe */  'p', 'e', 
      /* pf */  'p', 'f', 
      /* pi */  'p', 'i', 
      /* po */  'p', 'o', 
      /* privateuse */  'p', 'r', 'i', 'v', 'a', 't', 'e', 'u', 's', 'e', 
      /* ps */  'p', 's', 
      /* punctuation */  'p', 'u', 'n', 'c', 't', 'u', 'a', 't', 'i', 'o', 'n', 
      /* s* */  's', '*', 
      /* sc */  's', 'c', 
      /* separator */  's', 'e', 'p', 'a', 'r', 'a', 't', 'o', 'r', 
      /* sk */  's', 'k', 
      /* sm */  's', 'm', 
      /* so */  's', 'o', 
      /* spaceseparator */  's', 'p', 'a', 'c', 'e', 's', 'e', 'p', 'a', 'r', 'a', 't', 'o', 'r', 
      /* spacingcombiningmark */  's', 'p', 'a', 'c', 'i', 'n', 'g', 'c', 'o', 'm', 'b', 'i', 'n', 'i', 'n', 'g', 'm', 'a', 'r', 'k', 
      /* surrogate */  's', 'u', 'r', 'r', 'o', 'g', 'a', 't', 'e', 
      /* symbol */  's', 'y', 'm', 'b', 'o', 'l', 
      /* titlecase */  't', 'i', 't', 'l', 'e', 'c', 'a', 's', 'e', 
      /* titlecaseletter */  't', 'i', 't', 'l', 'e', 'c', 'a', 's', 'e', 'l', 'e', 't', 't', 'e', 'r', 
      /* uppercaseletter */  'u', 'p', 'p', 'e', 'r', 'c', 'a', 's', 'e', 'l', 'e', 't', 't', 'e', 'r', 
      /* z* */  'z', '*', 
      /* zl */  'z', 'l', 
      /* zp */  'z', 'p', 
      /* zs */  'z', 's', 
   };

   static const BOOST_REGEX_DETAIL_NS::character_pointer_range< ::UChar32> range_data[] = {
      { prop_name_table+0, prop_name_table+3, }, // any
      { prop_name_table+3, prop_name_table+8, }, // ascii
      { prop_name_table+8, prop_name_table+16, }, // assigned
      { prop_name_table+16, prop_name_table+18, }, // c*
      { prop_name_table+18, prop_name_table+20, }, // cc
      { prop_name_table+20, prop_name_table+22, }, // cf
      { prop_name_table+22, prop_name_table+38, }, // closepunctuation
      { prop_name_table+38, prop_name_table+40, }, // cn
      { prop_name_table+40, prop_name_table+42, }, // co
      { prop_name_table+42, prop_name_table+62, }, // connectorpunctuation
      { prop_name_table+62, prop_name_table+69, }, // control
      { prop_name_table+69, prop_name_table+71, }, // cs
      { prop_name_table+71, prop_name_table+85, }, // currencysymbol
      { prop_name_table+85, prop_name_table+100, }, // dashpunctuation
      { prop_name_table+100, prop_name_table+118, }, // decimaldigitnumber
      { prop_name_table+118, prop_name_table+131, }, // enclosingmark
      { prop_name_table+131, prop_name_table+147, }, // finalpunctuation
      { prop_name_table+147, prop_name_table+153, }, // format
      { prop_name_table+153, prop_name_table+171, }, // initialpunctuation
      { prop_name_table+171, prop_name_table+173, }, // l*
      { prop_name_table+173, prop_name_table+179, }, // letter
      { prop_name_table+179, prop_name_table+191, }, // letternumber
      { prop_name_table+191, prop_name_table+204, }, // lineseparator
      { prop_name_table+204, prop_name_table+206, }, // ll
      { prop_name_table+206, prop_name_table+208, }, // lm
      { prop_name_table+208, prop_name_table+210, }, // lo
      { prop_name_table+210, prop_name_table+225, }, // lowercaseletter
      { prop_name_table+225, prop_name_table+227, }, // lt
      { prop_name_table+227, prop_name_table+229, }, // lu
      { prop_name_table+229, prop_name_table+231, }, // m*
      { prop_name_table+231, prop_name_table+235, }, // mark
      { prop_name_table+235, prop_name_table+245, }, // mathsymbol
      { prop_name_table+245, prop_name_table+247, }, // mc
      { prop_name_table+247, prop_name_table+249, }, // me
      { prop_name_table+249, prop_name_table+251, }, // mn
      { prop_name_table+251, prop_name_table+265, }, // modifierletter
      { prop_name_table+265, prop_name_table+279, }, // modifiersymbol
      { prop_name_table+279, prop_name_table+281, }, // n*
      { prop_name_table+281, prop_name_table+283, }, // nd
      { prop_name_table+283, prop_name_table+285, }, // nl
      { prop_name_table+285, prop_name_table+287, }, // no
      { prop_name_table+287, prop_name_table+301, }, // nonspacingmark
      { prop_name_table+301, prop_name_table+312, }, // notassigned
      { prop_name_table+312, prop_name_table+318, }, // number
      { prop_name_table+318, prop_name_table+333, }, // openpunctuation
      { prop_name_table+333, prop_name_table+338, }, // other
      { prop_name_table+338, prop_name_table+349, }, // otherletter
      { prop_name_table+349, prop_name_table+360, }, // othernumber
      { prop_name_table+360, prop_name_table+376, }, // otherpunctuation
      { prop_name_table+376, prop_name_table+387, }, // othersymbol
      { prop_name_table+387, prop_name_table+389, }, // p*
      { prop_name_table+389, prop_name_table+407, }, // paragraphseparator
      { prop_name_table+407, prop_name_table+409, }, // pc
      { prop_name_table+409, prop_name_table+411, }, // pd
      { prop_name_table+411, prop_name_table+413, }, // pe
      { prop_name_table+413, prop_name_table+415, }, // pf
      { prop_name_table+415, prop_name_table+417, }, // pi
      { prop_name_table+417, prop_name_table+419, }, // po
      { prop_name_table+419, prop_name_table+429, }, // privateuse
      { prop_name_table+429, prop_name_table+431, }, // ps
      { prop_name_table+431, prop_name_table+442, }, // punctuation
      { prop_name_table+442, prop_name_table+444, }, // s*
      { prop_name_table+444, prop_name_table+446, }, // sc
      { prop_name_table+446, prop_name_table+455, }, // separator
      { prop_name_table+455, prop_name_table+457, }, // sk
      { prop_name_table+457, prop_name_table+459, }, // sm
      { prop_name_table+459, prop_name_table+461, }, // so
      { prop_name_table+461, prop_name_table+475, }, // spaceseparator
      { prop_name_table+475, prop_name_table+495, }, // spacingcombiningmark
      { prop_name_table+495, prop_name_table+504, }, // surrogate
      { prop_name_table+504, prop_name_table+510, }, // symbol
      { prop_name_table+510, prop_name_table+519, }, // titlecase
      { prop_name_table+519, prop_name_table+534, }, // titlecaseletter
      { prop_name_table+534, prop_name_table+549, }, // uppercaseletter
      { prop_name_table+549, prop_name_table+551, }, // z*
      { prop_name_table+551, prop_name_table+553, }, // zl
      { prop_name_table+553, prop_name_table+555, }, // zp
      { prop_name_table+555, prop_name_table+557, }, // zs
   };

   static const icu_regex_traits::char_class_type icu_class_map[] = {
      icu_regex_traits::mask_any, // any
      icu_regex_traits::mask_ascii, // ascii
      (0x3FFFFFFFu) & ~(U_GC_CN_MASK), // assigned
      U_GC_C_MASK, // c*
      U_GC_CC_MASK, // cc
      U_GC_CF_MASK, // cf
      U_GC_PE_MASK, // closepunctuation
      U_GC_CN_MASK, // cn
      U_GC_CO_MASK, // co
      U_GC_PC_MASK, // connectorpunctuation
      U_GC_CC_MASK, // control
      U_GC_CS_MASK, // cs
      U_GC_SC_MASK, // currencysymbol
      U_GC_PD_MASK, // dashpunctuation
      U_GC_ND_MASK, // decimaldigitnumber
      U_GC_ME_MASK, // enclosingmark
      U_GC_PF_MASK, // finalpunctuation
      U_GC_CF_MASK, // format
      U_GC_PI_MASK, // initialpunctuation
      U_GC_L_MASK, // l*
      U_GC_L_MASK, // letter
      U_GC_NL_MASK, // letternumber
      U_GC_ZL_MASK, // lineseparator
      U_GC_LL_MASK, // ll
      U_GC_LM_MASK, // lm
      U_GC_LO_MASK, // lo
      U_GC_LL_MASK, // lowercaseletter
      U_GC_LT_MASK, // lt
      U_GC_LU_MASK, // lu
      U_GC_M_MASK, // m*
      U_GC_M_MASK, // mark
      U_GC_SM_MASK, // mathsymbol
      U_GC_MC_MASK, // mc
      U_GC_ME_MASK, // me
      U_GC_MN_MASK, // mn
      U_GC_LM_MASK, // modifierletter
      U_GC_SK_MASK, // modifiersymbol
      U_GC_N_MASK, // n*
      U_GC_ND_MASK, // nd
      U_GC_NL_MASK, // nl
      U_GC_NO_MASK, // no
      U_GC_MN_MASK, // nonspacingmark
      U_GC_CN_MASK, // notassigned
      U_GC_N_MASK, // number
      U_GC_PS_MASK, // openpunctuation
      U_GC_C_MASK, // other
      U_GC_LO_MASK, // otherletter
      U_GC_NO_MASK, // othernumber
      U_GC_PO_MASK, // otherpunctuation
      U_GC_SO_MASK, // othersymbol
      U_GC_P_MASK, // p*
      U_GC_ZP_MASK, // paragraphseparator
      U_GC_PC_MASK, // pc
      U_GC_PD_MASK, // pd
      U_GC_PE_MASK, // pe
      U_GC_PF_MASK, // pf
      U_GC_PI_MASK, // pi
      U_GC_PO_MASK, // po
      U_GC_CO_MASK, // privateuse
      U_GC_PS_MASK, // ps
      U_GC_P_MASK, // punctuation
      U_GC_S_MASK, // s*
      U_GC_SC_MASK, // sc
      U_GC_Z_MASK, // separator
      U_GC_SK_MASK, // sk
      U_GC_SM_MASK, // sm
      U_GC_SO_MASK, // so
      U_GC_ZS_MASK, // spaceseparator
      U_GC_MC_MASK, // spacingcombiningmark
      U_GC_CS_MASK, // surrogate
      U_GC_S_MASK, // symbol
      U_GC_LT_MASK, // titlecase
      U_GC_LT_MASK, // titlecaseletter
      U_GC_LU_MASK, // uppercaseletter
      U_GC_Z_MASK, // z*
      U_GC_ZL_MASK, // zl
      U_GC_ZP_MASK, // zp
      U_GC_ZS_MASK, // zs
   };


   static const BOOST_REGEX_DETAIL_NS::character_pointer_range< ::UChar32>* ranges_begin = range_data;
   static const BOOST_REGEX_DETAIL_NS::character_pointer_range< ::UChar32>* ranges_end = range_data + (sizeof(range_data)/sizeof(range_data[0]));
   
   BOOST_REGEX_DETAIL_NS::character_pointer_range< ::UChar32> t = { p1, p2, };
   const BOOST_REGEX_DETAIL_NS::character_pointer_range< ::UChar32>* p = std::lower_bound(ranges_begin, ranges_end, t);
   if((p != ranges_end) && (t == *p))
      return icu_class_map[p - ranges_begin];
   return 0;
}

icu_regex_traits::char_class_type icu_regex_traits::lookup_classname(const char_type* p1, const char_type* p2) const
{
   static const char_class_type masks[] = 
   {
      0,
      U_GC_L_MASK | U_GC_ND_MASK, 
      U_GC_L_MASK,
      mask_blank,
      U_GC_CC_MASK | U_GC_CF_MASK | U_GC_ZL_MASK | U_GC_ZP_MASK,
      U_GC_ND_MASK,
      U_GC_ND_MASK,
      (0x3FFFFFFFu) & ~(U_GC_CC_MASK | U_GC_CF_MASK | U_GC_CS_MASK | U_GC_CN_MASK | U_GC_Z_MASK),
      mask_horizontal,
      U_GC_LL_MASK,
      U_GC_LL_MASK,
      ~(U_GC_C_MASK),
      U_GC_P_MASK,
      char_class_type(U_GC_Z_MASK) | mask_space,
      char_class_type(U_GC_Z_MASK) | mask_space,
      U_GC_LU_MASK,
      mask_unicode,
      U_GC_LU_MASK,
      mask_vertical,
      char_class_type(U_GC_L_MASK | U_GC_ND_MASK | U_GC_MN_MASK) | mask_underscore, 
      char_class_type(U_GC_L_MASK | U_GC_ND_MASK | U_GC_MN_MASK) | mask_underscore, 
      char_class_type(U_GC_ND_MASK) | mask_xdigit,
   };

   int idx = ::boost::BOOST_REGEX_DETAIL_NS::get_default_class_id(p1, p2);
   if(idx >= 0)
      return masks[idx+1];
   char_class_type result = lookup_icu_mask(p1, p2);
   if(result != 0)
      return result;

   if(idx < 0)
   {
      string_type s(p1, p2);
      string_type::size_type i = 0;
      while(i < s.size())
      {
         s[i] = static_cast<char>((::u_tolower)(s[i]));
         if(::u_isspace(s[i]) || (s[i] == '-') || (s[i] == '_'))
            s.erase(s.begin()+i, s.begin()+i+1);
         else
         {
            s[i] = static_cast<char>((::u_tolower)(s[i]));
            ++i;
         }
      }
      if(s.size())
         idx = ::boost::BOOST_REGEX_DETAIL_NS::get_default_class_id(&*s.begin(), &*s.begin() + s.size());
      if(idx >= 0)
         return masks[idx+1];
      if(s.size())
         result = lookup_icu_mask(&*s.begin(), &*s.begin() + s.size());
      if(result != 0)
         return result;
   }
   BOOST_ASSERT(std::size_t(idx+1) < sizeof(masks) / sizeof(masks[0]));
   return masks[idx+1];
}

icu_regex_traits::string_type icu_regex_traits::lookup_collatename(const char_type* p1, const char_type* p2) const
{
   string_type result;
#ifdef BOOST_NO_CXX98_BINDERS
   if(std::find_if(p1, p2, std::bind(std::greater< ::UChar32>(), std::placeholders::_1, 0x7f)) == p2)
#else
   if(std::find_if(p1, p2, std::bind2nd(std::greater< ::UChar32>(), 0x7f)) == p2)
#endif
   {
#ifndef BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS
      std::string s(p1, p2);
#else
      std::string s;
      const char_type* p3 = p1;
      while(p3 != p2)
         s.append(1, *p3++);
#endif
      // Try Unicode name:
      UErrorCode err = U_ZERO_ERROR;
      UChar32 c = ::u_charFromName(U_UNICODE_CHAR_NAME, s.c_str(), &err);
      if(U_SUCCESS(err))
      {
         result.push_back(c);
         return result;
      }
      // Try Unicode-extended name:
      err = U_ZERO_ERROR;
      c = ::u_charFromName(U_EXTENDED_CHAR_NAME, s.c_str(), &err);
      if(U_SUCCESS(err))
      {
         result.push_back(c);
         return result;
      }
      // try POSIX name:
      s = ::boost::BOOST_REGEX_DETAIL_NS::lookup_default_collate_name(s);
#ifndef BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS
      result.assign(s.begin(), s.end());
#else
      result.clear();
      std::string::const_iterator si, sj;
      si = s.begin();
      sj = s.end();
      while(si != sj)
         result.push_back(*si++);
#endif
   }
   if(result.empty() && (p2-p1 == 1))
      result.push_back(*p1);
   return result;
}

bool icu_regex_traits::isctype(char_type c, char_class_type f) const
{
   // check for standard catagories first:
   char_class_type m = char_class_type(static_cast<char_class_type>(1) << u_charType(c));
   if((m & f) != 0) 
      return true;
   // now check for special cases:
   if(((f & mask_blank) != 0) && u_isblank(c))
      return true;
   if(((f & mask_space) != 0) && u_isspace(c))
      return true;
   if(((f & mask_xdigit) != 0) && (u_digit(c, 16) >= 0))
      return true;
   if(((f & mask_unicode) != 0) && (c >= 0x100))
      return true;
   if(((f & mask_underscore) != 0) && (c == '_'))
      return true;
   if(((f & mask_any) != 0) && (c <= 0x10FFFF))
      return true;
   if(((f & mask_ascii) != 0) && (c <= 0x7F))
      return true;
   if(((f & mask_vertical) != 0) && (::boost::BOOST_REGEX_DETAIL_NS::is_separator(c) || (c == static_cast<char_type>('\v')) || (m == U_GC_ZL_MASK) || (m == U_GC_ZP_MASK)))
      return true;
   if(((f & mask_horizontal) != 0) && !::boost::BOOST_REGEX_DETAIL_NS::is_separator(c) && u_isspace(c) && (c != static_cast<char_type>('\v')))
      return true;
   return false;
}

}

#endif // BOOST_HAS_ICU
