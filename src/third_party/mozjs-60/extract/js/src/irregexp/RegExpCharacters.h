/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99: */

// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef V8_JSREGEXPCHARACTERS_H_
#define V8_JSREGEXPCHARACTERS_H_

namespace js {

namespace irregexp {

char16_t
ConvertNonLatin1ToLatin1(char16_t c, bool unicode);

// -------------------------------------------------------------------
// CharacterRange

// The ranges have inclusive from and exclusive to.

// This covers \s as defined in ES2016, 21.2.2.12 CharacterClassEscape,
// which includes WhiteSpace (11.2) and LineTerminator (11.3) values.
extern const int kSpaceRanges[];
extern const int kSpaceRangeCount;

// Characters in \s and additionally all surrogate characters.
extern const int kSpaceAndSurrogateRanges[];
extern const int kSpaceAndSurrogateRangeCount;

// This covers \w as defined in ES2016, 21.2.2.12 CharacterClassEscape.
extern const int kWordRanges[];
extern const int kWordRangeCount;

// Characters which case-fold to characters in \w.
extern const int kIgnoreCaseWordRanges[];
extern const int kIgnoreCaseWordRangeCount;

// Characters in \w and additionally all surrogate characters.
extern const int kWordAndSurrogateRanges[];
extern const int kWordAndSurrogateRangeCount;

// All characters excluding those which case-fold to \w and excluding all
// surrogate characters.
extern const int kNegatedIgnoreCaseWordAndSurrogateRanges[];
extern const int kNegatedIgnoreCaseWordAndSurrogateRangeCount;

// This covers \d as defined in ES2016, 21.2.2.12 CharacterClassEscape.
extern const int kDigitRanges[];
extern const int kDigitRangeCount;

// Characters in \d and additionally all surrogate characters.
extern const int kDigitAndSurrogateRanges[];
extern const int kDigitAndSurrogateRangeCount;

// The range of all surrogate characters.
extern const int kSurrogateRanges[];
extern const int kSurrogateRangeCount;

// Line terminators as defined in ES2016, 11.3 LineTerminator.
extern const int kLineTerminatorRanges[];
extern const int kLineTerminatorRangeCount;

// Line terminators and surrogate characters.
extern const int kLineTerminatorAndSurrogateRanges[];
extern const int kLineTerminatorAndSurrogateRangeCount;

} } // namespace js::irregexp

#endif // V8_JSREGEXPCHARACTERS_H_
