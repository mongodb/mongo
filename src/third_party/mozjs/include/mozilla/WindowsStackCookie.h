/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_WindowsStackCookie_h
#define mozilla_WindowsStackCookie_h

#if defined(DEBUG) && defined(_M_X64) && !defined(__MINGW64__)

#  include <windows.h>
#  include <winnt.h>

#  include <cstdint>

#  include "mozilla/Types.h"

namespace mozilla {

// This function does pattern matching on the instructions generated for a
// given function, to detect whether it uses stack buffers. More specifically,
// it looks for instructions that characterize the presence of stack cookie
// checks. When this function returns true, it can be a false positive, but we
// use a rather long pattern to make false positives very unlikely.
// Note: Do not use this function inside the function that lives at
//       aFunctionAddress, as that could introduce stack buffers.
// Note: The pattern we use does not work for MinGW builds.
inline bool HasStackCookieCheck(uintptr_t aFunctionAddress) {
  DWORD64 imageBase{};
  auto entry = ::RtlLookupFunctionEntry(
      reinterpret_cast<DWORD64>(aFunctionAddress), &imageBase, nullptr);
  if (entry && entry->EndAddress > entry->BeginAddress + 14) {
    auto begin = reinterpret_cast<uint8_t*>(imageBase + entry->BeginAddress);
    auto end = reinterpret_cast<uint8_t*>(imageBase + entry->EndAddress - 14);
    for (auto pc = begin; pc != end; ++pc) {
      // 48 8b 05 XX XX XX XX:      mov rax, qword ptr [rip + XXXXXXXX]
      if ((pc[0] == 0x48 && pc[1] == 0x8b && pc[2] == 0x05) &&
          // 48 31 e0:              xor rax, rsp
          (pc[7] == 0x48 && pc[8] == 0x31 && pc[9] == 0xe0) &&
          // 48 89 (8|4)4 24 ...:   mov qword ptr [rsp + ...], rax
          (pc[10] == 0x48 && pc[11] == 0x89 &&
           (pc[12] == 0x44 || pc[12] == 0x84) && pc[13] == 0x24)) {
        return true;
      }
    }
  }
  // In x64, if there is no entry, then there is no stack allocation, hence
  // there is no stack cookie check: "Table-based exception handling requires a
  // table entry for all functions that allocate stack space or call another
  // function (for example, nonleaf functions)."
  // https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64
  // Similarly, if the gap between begin and end is less than 14 bytes, then
  // the function cannot contain the pattern we are looking for, therefore it
  // has no cookie check either.
  return false;
}

}  // namespace mozilla

#endif  // defined(DEBUG) && defined(_M_X64) && !defined(__MINGW64__)

#endif  // mozilla_WindowsStackCookie_h
