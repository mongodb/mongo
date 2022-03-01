/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "mozilla/WindowsMapRemoteView.h"

#include "mozilla/Assertions.h"
#include "mozilla/DynamicallyLinkedFunctionPtr.h"

#include <winternl.h>

#if (NTDDI_VERSION < NTDDI_WIN10_RS2)

// MapViewOfFile2 is just an inline function that calls MapViewOfFileNuma2 with
// its preferred node set to NUMA_NO_PREFERRED_NODE
WINBASEAPI PVOID WINAPI MapViewOfFileNuma2(HANDLE aFileMapping, HANDLE aProcess,
                                           ULONG64 aOffset, PVOID aBaseAddress,
                                           SIZE_T aViewSize,
                                           ULONG aAllocationType,
                                           ULONG aPageProtection,
                                           ULONG aPreferredNode);

WINBASEAPI BOOL WINAPI UnmapViewOfFile2(HANDLE aProcess, PVOID aBaseAddress,
                                        ULONG aUnmapFlags);

#endif  // (NTDDI_VERSION < NTDDI_WIN10_RS2)

enum SECTION_INHERIT { ViewShare = 1, ViewUnmap = 2 };

NTSTATUS NTAPI NtMapViewOfSection(
    HANDLE aSection, HANDLE aProcess, PVOID* aBaseAddress, ULONG_PTR aZeroBits,
    SIZE_T aCommitSize, PLARGE_INTEGER aSectionOffset, PSIZE_T aViewSize,
    SECTION_INHERIT aInheritDisposition, ULONG aAllocationType,
    ULONG aProtectionFlags);

NTSTATUS NTAPI NtUnmapViewOfSection(HANDLE aProcess, PVOID aBaseAddress);

static DWORD GetWin32ErrorCode(NTSTATUS aNtStatus) {
  static const mozilla::StaticDynamicallyLinkedFunctionPtr<
      decltype(&RtlNtStatusToDosError)>
      pRtlNtStatusToDosError(L"ntdll.dll", "RtlNtStatusToDosError");

  MOZ_ASSERT(!!pRtlNtStatusToDosError);
  if (!pRtlNtStatusToDosError) {
    return ERROR_GEN_FAILURE;
  }

  return pRtlNtStatusToDosError(aNtStatus);
}

namespace mozilla {

MFBT_API void* MapRemoteViewOfFile(HANDLE aFileMapping, HANDLE aProcess,
                                   ULONG64 aOffset, PVOID aBaseAddress,
                                   SIZE_T aViewSize, ULONG aAllocationType,
                                   ULONG aProtectionFlags) {
  static const StaticDynamicallyLinkedFunctionPtr<decltype(&MapViewOfFileNuma2)>
      pMapViewOfFileNuma2(L"Api-ms-win-core-memory-l1-1-5.dll",
                          "MapViewOfFileNuma2");

  if (!!pMapViewOfFileNuma2) {
    return pMapViewOfFileNuma2(aFileMapping, aProcess, aOffset, aBaseAddress,
                               aViewSize, aAllocationType, aProtectionFlags,
                               NUMA_NO_PREFERRED_NODE);
  }

  static const StaticDynamicallyLinkedFunctionPtr<decltype(&NtMapViewOfSection)>
      pNtMapViewOfSection(L"ntdll.dll", "NtMapViewOfSection");

  MOZ_ASSERT(!!pNtMapViewOfSection);
  if (!pNtMapViewOfSection) {
    return nullptr;
  }

  // For the sake of consistency, we only permit the same flags that
  // MapViewOfFileNuma2 allows
  if (aAllocationType != 0 && aAllocationType != MEM_RESERVE &&
      aAllocationType != MEM_LARGE_PAGES) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }

  NTSTATUS ntStatus;

  LARGE_INTEGER offset;
  offset.QuadPart = aOffset;

  ntStatus = pNtMapViewOfSection(aFileMapping, aProcess, &aBaseAddress, 0, 0,
                                 &offset, &aViewSize, ViewUnmap,
                                 aAllocationType, aProtectionFlags);
  if (NT_SUCCESS(ntStatus)) {
    ::SetLastError(ERROR_SUCCESS);
    return aBaseAddress;
  }

  ::SetLastError(GetWin32ErrorCode(ntStatus));
  return nullptr;
}

MFBT_API bool UnmapRemoteViewOfFile(HANDLE aProcess, PVOID aBaseAddress) {
  static const StaticDynamicallyLinkedFunctionPtr<decltype(&UnmapViewOfFile2)>
      pUnmapViewOfFile2(L"kernel32.dll", "UnmapViewOfFile2");

  if (!!pUnmapViewOfFile2) {
    return !!pUnmapViewOfFile2(aProcess, aBaseAddress, 0);
  }

  static const StaticDynamicallyLinkedFunctionPtr<
      decltype(&NtUnmapViewOfSection)>
      pNtUnmapViewOfSection(L"ntdll.dll", "NtUnmapViewOfSection");

  MOZ_ASSERT(!!pNtUnmapViewOfSection);
  if (!pNtUnmapViewOfSection) {
    return false;
  }

  NTSTATUS ntStatus = pNtUnmapViewOfSection(aProcess, aBaseAddress);
  ::SetLastError(GetWin32ErrorCode(ntStatus));
  return NT_SUCCESS(ntStatus);
}

}  // namespace mozilla
