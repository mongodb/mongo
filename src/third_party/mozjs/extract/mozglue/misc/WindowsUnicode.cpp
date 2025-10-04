/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "WindowsUnicode.h"

#include <windows.h>
// For UNICODE_STRING
#include <winternl.h>

#include <string.h>

namespace mozilla {
namespace glue {

mozilla::UniquePtr<char[]> WideToUTF8(const wchar_t* aStr,
                                      const size_t aStrLenExclNul) {
  int numConv = ::WideCharToMultiByte(CP_UTF8, 0, aStr, aStrLenExclNul, nullptr,
                                      0, nullptr, nullptr);
  if (!numConv) {
    return nullptr;
  }

  // Include room for the null terminator by adding one
  auto buf = mozilla::MakeUnique<char[]>(numConv + 1);

  numConv = ::WideCharToMultiByte(CP_UTF8, 0, aStr, aStrLenExclNul, buf.get(),
                                  numConv, nullptr, nullptr);
  if (!numConv) {
    return nullptr;
  }

  // Add null termination. numConv does not include the terminator, so we don't
  // subtract 1 when indexing into buf.
  buf[numConv] = 0;

  return buf;
}

mozilla::UniquePtr<char[]> WideToUTF8(const wchar_t* aStr) {
  return WideToUTF8(aStr, wcslen(aStr));
}

mozilla::UniquePtr<char[]> WideToUTF8(const std::wstring& aStr) {
  return WideToUTF8(aStr.data(), aStr.length());
}

mozilla::UniquePtr<char[]> WideToUTF8(PCUNICODE_STRING aStr) {
  if (!aStr) {
    return nullptr;
  }

  return WideToUTF8(aStr->Buffer, aStr->Length / sizeof(WCHAR));
}

}  // namespace glue
}  // namespace mozilla
