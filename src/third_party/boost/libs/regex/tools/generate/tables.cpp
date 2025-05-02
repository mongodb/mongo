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
  *   FILE         tables.cpp
  *   VERSION      see <boost/version.hpp>
  *   DESCRIPTION: Generates code snippets programatically, for cut-and
  *                paste into regex source.
  */

#include <boost/config.hpp>
#include <map>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <cctype>

std::string g_char_type;
std::string g_data_type;
std::map<std::string, std::string> g_table;
std::map<std::string, std::pair<std::string, std::string> > g_help_table;

void add(std::string key, std::string data)
{
   g_table[key] = data;
   if(key.size() <= 2)
      g_help_table[data].first = key;
   else
      g_help_table[data].second = key;

   std::string::size_type i = 0;
   while(i < key.size())
   {
      if(std::isspace(key[i]) || (key[i] == '-') || (key[i] == '_'))
         key.erase(i, 1);
      else
      {
         key[i] = std::tolower(key[i]);
         ++i;
      }
   }
}

#define ADD(x, y) add(BOOST_STRINGIZE(x), BOOST_STRINGIZE(y))

void generate_code()
{
   std::map<std::string, std::string>::const_iterator i, j;

   // begin with the character tables:
   std::cout << "static const " << g_char_type << " prop_name_table[] = {\n";
   for(i = g_table.begin(), j = g_table.end(); i != j; ++i)
   {
      std::cout << "   /* " << i->first << " */  ";
      for(std::string::size_type n = 0; n < i->first.size(); ++n)
      {
         std::cout.put('\'');
         std::cout.put((i->first)[n]);
         std::cout.put('\'');
         std::cout.put(',');
         std::cout.put(' ');
      }
      std::cout << std::endl;;
   }
   std::cout << "};\n\n";

   // now the iterator table:
   std::cout << "static const BOOST_REGEX_DETAIL_NS::character_pointer_range<" << g_char_type << "> range_data[] = {\n";
   std::size_t index = 0;
   for(i = g_table.begin(), j = g_table.end(); i != j; ++i)
   {
      std::cout << "   { prop_name_table+" << index << ", prop_name_table+";
      index += i->first.size();
      std::cout << index << ", }, // " << i->first << std::endl;
   }
   std::cout << "};\n\n";

   // now the value table:
   std::cout << "static const " << g_data_type << " icu_class_map[] = {\n";
   for(i = g_table.begin(), j = g_table.end(); i != j; ++i)
   {
      std::cout << "   " << i->second << ", // " << i->first << std::endl;
   }
   std::cout << "};\n\n" << std::flush;
   g_table.clear();
}

void generate_html()
{
   // start by producing a sorted list:
   std::vector<std::pair<std::string, std::string> > v;
   std::map<std::string, std::pair<std::string, std::string> >::const_iterator i, j;
   i = g_help_table.begin();
   j = g_help_table.end();
   while(i != j)
   {
      v.push_back(i->second);
      ++i;
   }
   std::sort(v.begin(), v.end());

   std::vector<std::pair<std::string, std::string> >::const_iterator h, k;
   h = v.begin();
   k = v.end();

   std::cout << "<table width=\"100%\"><tr><td><b>Short Name</b></td><td><b>Long Name</b></td></tr>\n";
   while(h != k)
   {
      std::cout << "<tr><td>" << (h->first.size() ? h->first : std::string(" ")) << "</td><td>" << h->second << "</td></tr>\n";
      ++h;
   }
   std::cout << "</table>\n\n";
}

int main()
{
   g_char_type = "::UChar32";
   g_data_type = "icu_regex_traits::char_class_type";
   ADD(L*, U_GC_L_MASK); 
   ADD(Letter, U_GC_L_MASK); 
   ADD(Lu, U_GC_LU_MASK); 
   ADD(Ll, U_GC_LL_MASK); 
   ADD(Lt, U_GC_LT_MASK); 
   ADD(Lm, U_GC_LM_MASK); 
   ADD(Lo, U_GC_LO_MASK); 
   ADD(Uppercase Letter, U_GC_LU_MASK); 
   ADD(Lowercase Letter, U_GC_LL_MASK); 
   ADD(Titlecase Letter, U_GC_LT_MASK); 
   ADD(Modifier Letter, U_GC_LM_MASK); 
   ADD(Other Letter, U_GC_LO_MASK); 

   ADD(M*, U_GC_M_MASK); 
   ADD(Mn, U_GC_MN_MASK); 
   ADD(Mc, U_GC_MC_MASK); 
   ADD(Me, U_GC_ME_MASK); 
   ADD(Mark, U_GC_M_MASK); 
   ADD(Non-Spacing Mark, U_GC_MN_MASK); 
   ADD(Spacing Combining Mark, U_GC_MC_MASK); 
   ADD(Enclosing Mark, U_GC_ME_MASK); 

   ADD(N*, U_GC_N_MASK); 
   ADD(Nd, U_GC_ND_MASK); 
   ADD(Nl, U_GC_NL_MASK); 
   ADD(No, U_GC_NO_MASK);
   ADD(Number, U_GC_N_MASK); 
   ADD(Decimal Digit Number, U_GC_ND_MASK); 
   ADD(Letter Number, U_GC_NL_MASK); 
   ADD(Other Number, U_GC_NO_MASK);

   ADD(S*, U_GC_S_MASK); 
   ADD(Sm, U_GC_SM_MASK); 
   ADD(Sc, U_GC_SC_MASK); 
   ADD(Sk, U_GC_SK_MASK); 
   ADD(So, U_GC_SO_MASK); 
   ADD(Symbol, U_GC_S_MASK); 
   ADD(Math Symbol, U_GC_SM_MASK); 
   ADD(Currency Symbol, U_GC_SC_MASK); 
   ADD(Modifier Symbol, U_GC_SK_MASK); 
   ADD(Other Symbol, U_GC_SO_MASK); 

   ADD(P*, U_GC_P_MASK); 
   ADD(Pc, U_GC_PC_MASK); 
   ADD(Pd, U_GC_PD_MASK); 
   ADD(Ps, U_GC_PS_MASK); 
   ADD(Pe, U_GC_PE_MASK); 
   ADD(Pi, U_GC_PI_MASK); 
   ADD(Pf, U_GC_PF_MASK); 
   ADD(Po, U_GC_PO_MASK); 
   ADD(Punctuation, U_GC_P_MASK); 
   ADD(Connector Punctuation, U_GC_PC_MASK); 
   ADD(Dash Punctuation, U_GC_PD_MASK); 
   ADD(Open Punctuation, U_GC_PS_MASK); 
   ADD(Close Punctuation, U_GC_PE_MASK); 
   ADD(Initial Punctuation, U_GC_PI_MASK); 
   ADD(Final Punctuation, U_GC_PF_MASK); 
   ADD(Other Punctuation, U_GC_PO_MASK); 

   ADD(Z*, U_GC_Z_MASK); 
   ADD(Zs, U_GC_ZS_MASK); 
   ADD(Zl, U_GC_ZL_MASK); 
   ADD(Zp, U_GC_ZP_MASK); 
   ADD(Separator, U_GC_Z_MASK); 
   ADD(Space Separator, U_GC_ZS_MASK); 
   ADD(Line Separator, U_GC_ZL_MASK); 
   ADD(Paragraph Separator, U_GC_ZP_MASK); 

   ADD(C*, U_GC_C_MASK); 
   ADD(Cc, U_GC_CC_MASK); 
   ADD(Cf, U_GC_CF_MASK); 
   ADD(Cs, U_GC_CS_MASK); 
   ADD(Co, U_GC_CO_MASK); 
   ADD(Cn, U_GC_CN_MASK); 
   ADD(Other, U_GC_C_MASK); 
   ADD(Control, U_GC_CC_MASK); 
   ADD(Format, U_GC_CF_MASK); 
   ADD(Surrogate, U_GC_CS_MASK); 
   ADD(Private Use, U_GC_CO_MASK); 
   ADD(Not Assigned, U_GC_CN_MASK); 
   ADD(Any, icu_regex_traits::mask_any); 
   ADD(Assigned, (0x3FFFFFFFu) & ~(U_GC_CN_MASK)); 
   ADD(ASCII, icu_regex_traits::mask_ascii); 
   ADD(Titlecase, U_GC_LT_MASK); 

   generate_code();
   generate_html();
   return 0;
}
