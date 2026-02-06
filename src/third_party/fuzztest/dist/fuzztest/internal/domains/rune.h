/*
 * The authors of this software are Rob Pike and Ken Thompson.
 *              Copyright (c) 2002 by Lucent Technologies.
 * Permission to use, copy, modify, and distribute this software for any
 * purpose without fee is hereby granted, provided that this entire notice
 * is included in all copies of any software which is or includes a copy
 * or modification of this software and in all copies of the supporting
 * documentation for such software.
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTY.  IN PARTICULAR, NEITHER THE AUTHORS NOR LUCENT TECHNOLOGIES MAKE
 * ANY REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY OF
 * THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 * rune.* have been converted to compile as C++ code in fuzztest::internal
 * namespace.
 */

#ifndef FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_RUNE_H_
#define FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_RUNE_H_

namespace fuzztest::internal {

typedef signed int Rune; /* Code-point values in Unicode 4.0 are 21 bits wide.*/

enum {
  UTFmax = 4,         /* maximum bytes per rune */
  Runesync = 0x80,    /* cannot represent part of a UTF sequence (<) */
  Runeself = 0x80,    /* rune and UTF sequences are the same (<) */
  Runeerror = 0xFFFD, /* decoding error in UTF */
  Runemax = 0x10FFFF, /* maximum rune value */
};

int runetochar(char* s, const Rune* r);
int chartorune(Rune* r, const char* s);

}  // namespace fuzztest::internal

#endif  // FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_RUNE_H_
