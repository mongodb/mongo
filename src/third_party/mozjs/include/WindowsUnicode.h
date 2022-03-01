/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_glue_WindowsUnicode_h
#define mozilla_glue_WindowsUnicode_h

#include "mozilla/UniquePtr.h"

#include <string>

struct _UNICODE_STRING;

namespace mozilla {
namespace glue {

mozilla::UniquePtr<char[]> WideToUTF8(const wchar_t* aStr,
                                      const size_t aStrLenExclNul);

mozilla::UniquePtr<char[]> WideToUTF8(const wchar_t* aStr);
mozilla::UniquePtr<char[]> WideToUTF8(const std::wstring& aStr);
mozilla::UniquePtr<char[]> WideToUTF8(const _UNICODE_STRING* aStr);

#if defined(bstr_t)
inline mozilla::UniquePtr<char[]> WideToUTF8(const _bstr_t& aStr) {
  return WideToUTF8(static_cast<const wchar_t*>(aStr), aStr.length());
}
#endif  // defined(bstr_t)

}  // namespace glue
}  // namespace mozilla

#endif  // mozilla_glue_WindowsUnicode_h
