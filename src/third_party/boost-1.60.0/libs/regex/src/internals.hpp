/*
 *
 * Copyright (c) 2011
 * John Maddock
 *
 * Use, modification and distribution are subject to the 
 * Boost Software License, Version 1.0. (See accompanying file 
 * LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 *
 */

#ifndef BOOST_REGEX_SRC_INTERNALS_HPP
#define BOOST_REGEX_SRC_INTERNALS_HPP

enum
{
   char_class_space=1<<0, 
   char_class_print=1<<1, 
   char_class_cntrl=1<<2, 
   char_class_upper=1<<3, 
   char_class_lower=1<<4,
   char_class_alpha=1<<5, 
   char_class_digit=1<<6, 
   char_class_punct=1<<7, 
   char_class_xdigit=1<<8,
   char_class_alnum=char_class_alpha|char_class_digit, 
   char_class_graph=char_class_alnum|char_class_punct,
   char_class_blank=1<<9,
   char_class_word=1<<10,
   char_class_unicode=1<<11,
   char_class_horizontal=1<<12,
   char_class_vertical=1<<13
};

#endif // BOOST_REGEX_SRC_INTERNALS_HPP
