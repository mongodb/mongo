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
  *   FILE         regex_traits_defaults.cpp
  *   VERSION      see <boost/version.hpp>
  *   DESCRIPTION: Declares API's for access to regex_traits default properties.
  */

#define BOOST_REGEX_SOURCE
#include <boost/regex/regex_traits.hpp>

#include <cctype>
#ifndef BOOST_NO_WREGEX
#include <cwctype>
#endif

#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{
   using ::tolower;
   using ::toupper;
#ifndef BOOST_NO_WREGEX
   using ::towlower;
   using ::towupper;
#endif
}
#endif


namespace boost{ namespace BOOST_REGEX_DETAIL_NS{

BOOST_REGEX_DECL const char* BOOST_REGEX_CALL get_default_syntax(regex_constants::syntax_type n)
{
   // if the user hasn't supplied a message catalog, then this supplies
   // default "messages" for us to load in the range 1-100.
   const char* messages[] = {
         "",
         "(",
         ")",
         "$",
         "^",
         ".",
         "*",
         "+",
         "?",
         "[",
         "]",
         "|",
         "\\",
         "#",
         "-",
         "{",
         "}",
         "0123456789",
         "b",
         "B",
         "<",
         ">",
         "",
         "",
         "A`",
         "z'",
         "\n",
         ",",
         "a",
         "f",
         "n",
         "r",
         "t",
         "v",
         "x",
         "c",
         ":",
         "=",
         "e",
         "",
         "",
         "",
         "",
         "",
         "",
         "",
         "",
         "E",
         "Q",
         "X",
         "C",
         "Z",
         "G",
         "!",
         "p",
         "P",
         "N",
         "gk",
         "K",
         "R",
   };

   return ((n >= (sizeof(messages) / sizeof(messages[1]))) ? "" : messages[n]);
}

BOOST_REGEX_DECL const char* BOOST_REGEX_CALL get_default_error_string(regex_constants::error_type n)
{
   static const char* const s_default_error_messages[] = {
      "Success",                                                            /* REG_NOERROR 0 error_ok */
      "No match",                                                           /* REG_NOMATCH 1 error_no_match */
      "Invalid regular expression.",                                        /* REG_BADPAT 2 error_bad_pattern */
      "Invalid collation character.",                                       /* REG_ECOLLATE 3 error_collate */
      "Invalid character class name, collating name, or character range.",  /* REG_ECTYPE 4 error_ctype */
      "Invalid or unterminated escape sequence.",                           /* REG_EESCAPE 5 error_escape */
      "Invalid back reference: specified capturing group does not exist.",  /* REG_ESUBREG 6 error_backref */
      "Unmatched [ or [^ in character class declaration.",                  /* REG_EBRACK 7 error_brack */
      "Unmatched marking parenthesis ( or \\(.",                            /* REG_EPAREN 8 error_paren */
      "Unmatched quantified repeat operator { or \\{.",                     /* REG_EBRACE 9 error_brace */
      "Invalid content of repeat range.",                                   /* REG_BADBR 10 error_badbrace */
      "Invalid range end in character class",                               /* REG_ERANGE 11 error_range */
      "Out of memory.",                                                     /* REG_ESPACE 12 error_space NOT USED */
      "Invalid preceding regular expression prior to repetition operator.", /* REG_BADRPT 13 error_badrepeat */
      "Premature end of regular expression",                                /* REG_EEND 14 error_end NOT USED */
      "Regular expression is too large.",                                   /* REG_ESIZE 15 error_size NOT USED */
      "Unmatched ) or \\)",                                                 /* REG_ERPAREN 16 error_right_paren NOT USED */
      "Empty regular expression.",                                          /* REG_EMPTY 17 error_empty */
      "The complexity of matching the regular expression exceeded predefined bounds.  "
      "Try refactoring the regular expression to make each choice made by the state machine unambiguous.  "
      "This exception is thrown to prevent \"eternal\" matches that take an "
      "indefinite period time to locate.",                                  /* REG_ECOMPLEXITY 18 error_complexity */
      "Ran out of stack space trying to match the regular expression.",     /* REG_ESTACK 19 error_stack */
      "Invalid or unterminated Perl (?...) sequence.",                      /* REG_E_PERL 20 error_perl */
      "Unknown error.",                                                     /* REG_E_UNKNOWN 21 error_unknown */
   };

   return (n > ::boost::regex_constants::error_unknown) ? s_default_error_messages[ ::boost::regex_constants::error_unknown] : s_default_error_messages[n];
}

BOOST_REGEX_DECL bool BOOST_REGEX_CALL is_combining_implementation(boost::uint_least16_t c)
{
   const boost::uint_least16_t combining_ranges[] = { 0x0300, 0x0361, 
                           0x0483, 0x0486, 
                           0x0903, 0x0903, 
                           0x093E, 0x0940, 
                           0x0949, 0x094C,
                           0x0982, 0x0983,
                           0x09BE, 0x09C0,
                           0x09C7, 0x09CC,
                           0x09D7, 0x09D7,
                           0x0A3E, 0x0A40,
                           0x0A83, 0x0A83,
                           0x0ABE, 0x0AC0,
                           0x0AC9, 0x0ACC,
                           0x0B02, 0x0B03,
                           0x0B3E, 0x0B3E,
                           0x0B40, 0x0B40,
                           0x0B47, 0x0B4C,
                           0x0B57, 0x0B57,
                           0x0B83, 0x0B83,
                           0x0BBE, 0x0BBF,
                           0x0BC1, 0x0BCC,
                           0x0BD7, 0x0BD7,
                           0x0C01, 0x0C03,
                           0x0C41, 0x0C44,
                           0x0C82, 0x0C83,
                           0x0CBE, 0x0CBE,
                           0x0CC0, 0x0CC4,
                           0x0CC7, 0x0CCB,
                           0x0CD5, 0x0CD6,
                           0x0D02, 0x0D03,
                           0x0D3E, 0x0D40,
                           0x0D46, 0x0D4C,
                           0x0D57, 0x0D57,
                           0x0F7F, 0x0F7F,
                           0x20D0, 0x20E1, 
                           0x3099, 0x309A,
                           0xFE20, 0xFE23, 
                           0xffff, 0xffff, };

      const boost::uint_least16_t* p = combining_ranges + 1;
   while(*p < c) p += 2;
   --p;
   if((c >= *p) && (c <= *(p+1)))
         return true;
   return false;
}

//
// these are the POSIX collating names:
//
BOOST_REGEX_DECL const char* def_coll_names[] = {
"NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "alert", "backspace", "tab", "newline", 
"vertical-tab", "form-feed", "carriage-return", "SO", "SI", "DLE", "DC1", "DC2", "DC3", "DC4", "NAK", 
"SYN", "ETB", "CAN", "EM", "SUB", "ESC", "IS4", "IS3", "IS2", "IS1", "space", "exclamation-mark", 
"quotation-mark", "number-sign", "dollar-sign", "percent-sign", "ampersand", "apostrophe", 
"left-parenthesis", "right-parenthesis", "asterisk", "plus-sign", "comma", "hyphen", 
"period", "slash", "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", 
"colon", "semicolon", "less-than-sign", "equals-sign", "greater-than-sign", 
"question-mark", "commercial-at", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", 
"Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "left-square-bracket", "backslash", 
"right-square-bracket", "circumflex", "underscore", "grave-accent", "a", "b", "c", "d", "e", "f", 
"g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "left-curly-bracket", 
"vertical-line", "right-curly-bracket", "tilde", "DEL", "", 
};

// these multi-character collating elements
// should keep most Western-European locales
// happy - we should really localise these a
// little more - but this will have to do for
// now:

BOOST_REGEX_DECL const char* def_multi_coll[] = {
   "ae",
   "Ae",
   "AE",
   "ch",
   "Ch",
   "CH",
   "ll",
   "Ll",
   "LL",
   "ss",
   "Ss",
   "SS",
   "nj",
   "Nj",
   "NJ",
   "dz",
   "Dz",
   "DZ",
   "lj",
   "Lj",
   "LJ",
   "",
};



BOOST_REGEX_DECL std::string BOOST_REGEX_CALL lookup_default_collate_name(const std::string& name)
{
   unsigned int i = 0;
   while(*def_coll_names[i])
   {
      if(def_coll_names[i] == name)
      {
         return std::string(1, char(i));
      }
      ++i;
   }
   i = 0;
   while(*def_multi_coll[i])
   {
      if(def_multi_coll[i] == name)
      {
         return def_multi_coll[i];
      }
      ++i;
   }
   return std::string();
}

BOOST_REGEX_DECL char BOOST_REGEX_CALL do_global_lower(char c)
{
   return static_cast<char>((std::tolower)((unsigned char)c));
}

BOOST_REGEX_DECL char BOOST_REGEX_CALL do_global_upper(char c)
{
   return static_cast<char>((std::toupper)((unsigned char)c));
}
#ifndef BOOST_NO_WREGEX
BOOST_REGEX_DECL wchar_t BOOST_REGEX_CALL do_global_lower(wchar_t c)
{
   return (std::towlower)(c);
}

BOOST_REGEX_DECL wchar_t BOOST_REGEX_CALL do_global_upper(wchar_t c)
{
   return (std::towupper)(c);
}
#ifdef BOOST_REGEX_HAS_OTHER_WCHAR_T
BOOST_REGEX_DECL unsigned short BOOST_REGEX_CALL do_global_lower(unsigned short c)
{
   return (std::towlower)(c);
}

BOOST_REGEX_DECL unsigned short BOOST_REGEX_CALL do_global_upper(unsigned short c)
{
   return (std::towupper)(c);
}
#endif

#endif

BOOST_REGEX_DECL regex_constants::escape_syntax_type BOOST_REGEX_CALL get_default_escape_syntax_type(char c)
{
   //
   // char_syntax determines how the compiler treats a given character
   // in a regular expression.
   //
   static regex_constants::escape_syntax_type char_syntax[] = {
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,     /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /* */    // 32
      regex_constants::escape_type_identity,        /*!*/
      regex_constants::escape_type_identity,        /*"*/
      regex_constants::escape_type_identity,        /*#*/
      regex_constants::escape_type_identity,        /*$*/
      regex_constants::escape_type_identity,        /*%*/
      regex_constants::escape_type_identity,        /*&*/
      regex_constants::escape_type_end_buffer,        /*'*/
      regex_constants::syntax_open_mark,        /*(*/
      regex_constants::syntax_close_mark,        /*)*/
      regex_constants::escape_type_identity,        /***/
      regex_constants::syntax_plus,                 /*+*/
      regex_constants::escape_type_identity,        /*,*/
      regex_constants::escape_type_identity,        /*-*/
      regex_constants::escape_type_identity,        /*.*/
      regex_constants::escape_type_identity,        /*/*/
      regex_constants::escape_type_decimal,        /*0*/
      regex_constants::escape_type_backref,        /*1*/
      regex_constants::escape_type_backref,        /*2*/
      regex_constants::escape_type_backref,        /*3*/
      regex_constants::escape_type_backref,        /*4*/
      regex_constants::escape_type_backref,        /*5*/
      regex_constants::escape_type_backref,        /*6*/
      regex_constants::escape_type_backref,        /*7*/
      regex_constants::escape_type_backref,        /*8*/
      regex_constants::escape_type_backref,        /*9*/
      regex_constants::escape_type_identity,        /*:*/
      regex_constants::escape_type_identity,        /*;*/
      regex_constants::escape_type_left_word,        /*<*/
      regex_constants::escape_type_identity,        /*=*/
      regex_constants::escape_type_right_word,        /*>*/
      regex_constants::syntax_question,              /*?*/
      regex_constants::escape_type_identity,         /*@*/
      regex_constants::escape_type_start_buffer,     /*A*/
      regex_constants::escape_type_not_word_assert,  /*B*/
      regex_constants::escape_type_C,                /*C*/
      regex_constants::escape_type_not_class,        /*D*/
      regex_constants::escape_type_E,                /*E*/
      regex_constants::escape_type_not_class,        /*F*/
      regex_constants::escape_type_G,                /*G*/
      regex_constants::escape_type_not_class,        /*H*/
      regex_constants::escape_type_not_class,        /*I*/
      regex_constants::escape_type_not_class,        /*J*/
      regex_constants::escape_type_reset_start_mark, /*K*/
      regex_constants::escape_type_not_class,        /*L*/
      regex_constants::escape_type_not_class,        /*M*/
      regex_constants::escape_type_named_char,       /*N*/
      regex_constants::escape_type_not_class,        /*O*/
      regex_constants::escape_type_not_property,     /*P*/
      regex_constants::escape_type_Q,                /*Q*/
      regex_constants::escape_type_line_ending,      /*R*/
      regex_constants::escape_type_not_class,        /*S*/
      regex_constants::escape_type_not_class,        /*T*/
      regex_constants::escape_type_not_class,        /*U*/
      regex_constants::escape_type_not_class,        /*V*/
      regex_constants::escape_type_not_class,        /*W*/
      regex_constants::escape_type_X,                /*X*/
      regex_constants::escape_type_not_class,        /*Y*/
      regex_constants::escape_type_Z,                /*Z*/
      regex_constants::escape_type_identity,        /*[*/
      regex_constants::escape_type_identity,        /*\*/
      regex_constants::escape_type_identity,        /*]*/
      regex_constants::escape_type_identity,        /*^*/
      regex_constants::escape_type_identity,        /*_*/
      regex_constants::escape_type_start_buffer,        /*`*/
      regex_constants::escape_type_control_a,        /*a*/
      regex_constants::escape_type_word_assert,        /*b*/
      regex_constants::escape_type_ascii_control,        /*c*/
      regex_constants::escape_type_class,        /*d*/
      regex_constants::escape_type_e,        /*e*/
      regex_constants::escape_type_control_f,       /*f*/
      regex_constants::escape_type_extended_backref,  /*g*/
      regex_constants::escape_type_class,        /*h*/
      regex_constants::escape_type_class,        /*i*/
      regex_constants::escape_type_class,        /*j*/
      regex_constants::escape_type_extended_backref, /*k*/
      regex_constants::escape_type_class,        /*l*/
      regex_constants::escape_type_class,        /*m*/
      regex_constants::escape_type_control_n,       /*n*/
      regex_constants::escape_type_class,           /*o*/
      regex_constants::escape_type_property,        /*p*/
      regex_constants::escape_type_class,           /*q*/
      regex_constants::escape_type_control_r,       /*r*/
      regex_constants::escape_type_class,           /*s*/
      regex_constants::escape_type_control_t,       /*t*/
      regex_constants::escape_type_class,         /*u*/
      regex_constants::escape_type_control_v,       /*v*/
      regex_constants::escape_type_class,           /*w*/
      regex_constants::escape_type_hex,             /*x*/
      regex_constants::escape_type_class,           /*y*/
      regex_constants::escape_type_end_buffer,      /*z*/
      regex_constants::syntax_open_brace,           /*{*/
      regex_constants::syntax_or,                   /*|*/
      regex_constants::syntax_close_brace,          /*}*/
      regex_constants::escape_type_identity,        /*~*/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
      regex_constants::escape_type_identity,        /**/
   };

   return char_syntax[(unsigned char)c];
}

BOOST_REGEX_DECL regex_constants::syntax_type BOOST_REGEX_CALL get_default_syntax_type(char c)
{
   //
   // char_syntax determines how the compiler treats a given character
   // in a regular expression.
   //
   static regex_constants::syntax_type char_syntax[] = {
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_newline,     /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /* */    // 32
      regex_constants::syntax_not,        /*!*/
      regex_constants::syntax_char,        /*"*/
      regex_constants::syntax_hash,        /*#*/
      regex_constants::syntax_dollar,        /*$*/
      regex_constants::syntax_char,        /*%*/
      regex_constants::syntax_char,        /*&*/
      regex_constants::escape_type_end_buffer,  /*'*/
      regex_constants::syntax_open_mark,        /*(*/
      regex_constants::syntax_close_mark,        /*)*/
      regex_constants::syntax_star,        /***/
      regex_constants::syntax_plus,        /*+*/
      regex_constants::syntax_comma,        /*,*/
      regex_constants::syntax_dash,        /*-*/
      regex_constants::syntax_dot,        /*.*/
      regex_constants::syntax_char,        /*/*/
      regex_constants::syntax_digit,        /*0*/
      regex_constants::syntax_digit,        /*1*/
      regex_constants::syntax_digit,        /*2*/
      regex_constants::syntax_digit,        /*3*/
      regex_constants::syntax_digit,        /*4*/
      regex_constants::syntax_digit,        /*5*/
      regex_constants::syntax_digit,        /*6*/
      regex_constants::syntax_digit,        /*7*/
      regex_constants::syntax_digit,        /*8*/
      regex_constants::syntax_digit,        /*9*/
      regex_constants::syntax_colon,        /*:*/
      regex_constants::syntax_char,        /*;*/
      regex_constants::escape_type_left_word, /*<*/
      regex_constants::syntax_equal,        /*=*/
      regex_constants::escape_type_right_word, /*>*/
      regex_constants::syntax_question,        /*?*/
      regex_constants::syntax_char,        /*@*/
      regex_constants::syntax_char,        /*A*/
      regex_constants::syntax_char,        /*B*/
      regex_constants::syntax_char,        /*C*/
      regex_constants::syntax_char,        /*D*/
      regex_constants::syntax_char,        /*E*/
      regex_constants::syntax_char,        /*F*/
      regex_constants::syntax_char,        /*G*/
      regex_constants::syntax_char,        /*H*/
      regex_constants::syntax_char,        /*I*/
      regex_constants::syntax_char,        /*J*/
      regex_constants::syntax_char,        /*K*/
      regex_constants::syntax_char,        /*L*/
      regex_constants::syntax_char,        /*M*/
      regex_constants::syntax_char,        /*N*/
      regex_constants::syntax_char,        /*O*/
      regex_constants::syntax_char,        /*P*/
      regex_constants::syntax_char,        /*Q*/
      regex_constants::syntax_char,        /*R*/
      regex_constants::syntax_char,        /*S*/
      regex_constants::syntax_char,        /*T*/
      regex_constants::syntax_char,        /*U*/
      regex_constants::syntax_char,        /*V*/
      regex_constants::syntax_char,        /*W*/
      regex_constants::syntax_char,        /*X*/
      regex_constants::syntax_char,        /*Y*/
      regex_constants::syntax_char,        /*Z*/
      regex_constants::syntax_open_set,        /*[*/
      regex_constants::syntax_escape,        /*\*/
      regex_constants::syntax_close_set,        /*]*/
      regex_constants::syntax_caret,        /*^*/
      regex_constants::syntax_char,        /*_*/
      regex_constants::syntax_char,        /*`*/
      regex_constants::syntax_char,        /*a*/
      regex_constants::syntax_char,        /*b*/
      regex_constants::syntax_char,        /*c*/
      regex_constants::syntax_char,        /*d*/
      regex_constants::syntax_char,        /*e*/
      regex_constants::syntax_char,        /*f*/
      regex_constants::syntax_char,        /*g*/
      regex_constants::syntax_char,        /*h*/
      regex_constants::syntax_char,        /*i*/
      regex_constants::syntax_char,        /*j*/
      regex_constants::syntax_char,        /*k*/
      regex_constants::syntax_char,        /*l*/
      regex_constants::syntax_char,        /*m*/
      regex_constants::syntax_char,        /*n*/
      regex_constants::syntax_char,        /*o*/
      regex_constants::syntax_char,        /*p*/
      regex_constants::syntax_char,        /*q*/
      regex_constants::syntax_char,        /*r*/
      regex_constants::syntax_char,        /*s*/
      regex_constants::syntax_char,        /*t*/
      regex_constants::syntax_char,        /*u*/
      regex_constants::syntax_char,        /*v*/
      regex_constants::syntax_char,        /*w*/
      regex_constants::syntax_char,        /*x*/
      regex_constants::syntax_char,        /*y*/
      regex_constants::syntax_char,        /*z*/
      regex_constants::syntax_open_brace,        /*{*/
      regex_constants::syntax_or,        /*|*/
      regex_constants::syntax_close_brace,        /*}*/
      regex_constants::syntax_char,        /*~*/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
      regex_constants::syntax_char,        /**/
   };

   return char_syntax[(unsigned char)c];
}


} // BOOST_REGEX_DETAIL_NS
} // boost
