/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * MOZ_LIKELY and MOZ_UNLIKELY macros to hint to the compiler how a
 * boolean predicate should be branch-predicted.
 */

#ifndef mozilla_Likely_h
#define mozilla_Likely_h

#if defined(__clang__) || defined(__GNUC__)
#  define MOZ_LIKELY(x)   (__builtin_expect(!!(x), 1))
#  define MOZ_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#  define MOZ_LIKELY(x)   (!!(x))
#  define MOZ_UNLIKELY(x) (!!(x))
#endif

#endif /* mozilla_Likely_h */
