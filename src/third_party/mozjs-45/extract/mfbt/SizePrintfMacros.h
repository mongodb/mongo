/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implements (nonstandard) PRI{ouxX}SIZE format macros for size_t types. */

#ifndef mozilla_SizePrintfMacros_h_
#define mozilla_SizePrintfMacros_h_

/*
 * MSVC's libc does not support C99's %z format length modifier for size_t
 * types. Instead, we use Microsoft's nonstandard %I modifier for size_t, which
 * is unsigned __int32 on 32-bit platforms and unsigned __int64 on 64-bit
 * platforms:
 *
 * http://msdn.microsoft.com/en-us/library/tcxf1dw6.aspx
 */

#if defined(XP_WIN)
#  define PRIoSIZE  "Io"
#  define PRIuSIZE  "Iu"
#  define PRIxSIZE  "Ix"
#  define PRIXSIZE  "IX"
#else
#  define PRIoSIZE  "zo"
#  define PRIuSIZE  "zu"
#  define PRIxSIZE  "zx"
#  define PRIXSIZE  "zX"
#endif

#endif  /* mozilla_SizePrintfMacros_h_ */
