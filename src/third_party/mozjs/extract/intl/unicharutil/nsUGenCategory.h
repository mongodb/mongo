/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsUGenCategory_h
#define nsUGenCategory_h

/**
 *  Read http://unicode.org/reports/tr44/#General_Category_Values
 *  for the detailed definition of the following categories
 */
enum class nsUGenCategory {
  kUndefined = 0,
  kMark = 1,         // Mn, Mc, and Me
  kNumber = 2,       // Nd, Nl, and No
  kSeparator = 3,    // Zs, Zl, and Zp
  kOther = 4,        // Cc, Cf, Cs, Co, and Cn
  kLetter = 5,       // Lu, Ll, Lt, Lm, and Lo
  kPunctuation = 6,  // Pc, Pd, Ps, Pe, Pi, Pf, and Po
  kSymbol = 7        // Sm, Sc, Sk, and So
};

#endif  // nsUGenCategory_h
