/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Helpers for units on integer literals. */

#ifndef mozilla_Literals_h
#define mozilla_Literals_h

#include <cstddef>

// User-defined literals to make constants more legible.  Use them by
// appending them to literals such as:
//
// size_t page_size = 4_KiB;
//
constexpr size_t operator"" _KiB(unsigned long long int aNum) {
  return size_t(aNum) * 1024;
}

constexpr size_t operator"" _KiB(long double aNum) {
  return size_t(aNum * 1024);
}

constexpr size_t operator"" _MiB(unsigned long long int aNum) {
  return size_t(aNum) * 1024_KiB;
}

constexpr size_t operator"" _MiB(long double aNum) {
  return size_t(aNum * 1024_KiB);
}

constexpr double operator""_percent(long double aPercent) {
  return double(aPercent) / 100;
}

#endif /* ! mozilla_Literals_h */
