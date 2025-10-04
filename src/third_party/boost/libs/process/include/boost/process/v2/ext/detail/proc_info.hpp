// Copyright (c) 2022 Klemens D. Morgenstern
// Copyright (c) 2022 Samuel Venable
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_DETAIL_PROC_INFO_HPP
#define BOOST_PROCESS_V2_DETAIL_PROC_INFO_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/detail/throw_error.hpp>
#include <boost/process/v2/pid.hpp>

#include <string>
#include <vector>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <iterator>
#include <algorithm>
#include <windows.h>
#include <winternl.h>
extern "C" ULONG NTAPI RtlNtStatusToDosError(NTSTATUS Status);
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace detail
{

namespace ext 
{

#if defined(BOOST_PROCESS_V2_WINDOWS)
#if !defined(_MSC_VER)
#pragma pack(push, 8)
#else
#include <pshpack8.h>
#endif

/* CURDIR struct from:
 https://github.com/processhacker/phnt/
 CC BY 4.0 licence */

typedef struct {
  UNICODE_STRING DosPath;
  HANDLE Handle;
} CURDIR;

/* RTL_DRIVE_LETTER_CURDIR struct from:
 https://github.com/processhacker/phnt/
 CC BY 4.0 licence */

typedef struct {
  USHORT Flags;
  USHORT Length;
  ULONG TimeStamp;
  STRING DosPath;
} RTL_DRIVE_LETTER_CURDIR;

/* RTL_USER_PROCESS_PARAMETERS struct from:
 https://github.com/processhacker/phnt/
 CC BY 4.0 licence */

typedef struct {
  ULONG MaximumLength;
  ULONG Length;
  ULONG Flags;
  ULONG DebugFlags;
  HANDLE ConsoleHandle;
  ULONG ConsoleFlags;
  HANDLE StandardInput;
  HANDLE StandardOutput;
  HANDLE StandardError;
  CURDIR CurrentDirectory;
  UNICODE_STRING DllPath;
  UNICODE_STRING ImagePathName;
  UNICODE_STRING CommandLine;
  PVOID Environment;
  ULONG StartingX;
  ULONG StartingY;
  ULONG CountX;
  ULONG CountY;
  ULONG CountCharsX;
  ULONG CountCharsY;
  ULONG FillAttribute;
  ULONG WindowFlags;
  ULONG ShowWindowFlags;
  UNICODE_STRING WindowTitle;
  UNICODE_STRING DesktopInfo;
  UNICODE_STRING ShellInfo;
  UNICODE_STRING RuntimeData;
  RTL_DRIVE_LETTER_CURDIR CurrentDirectories[32];
  ULONG_PTR EnvironmentSize;
  ULONG_PTR EnvironmentVersion;
  PVOID PackageDependencyData;
  ULONG ProcessGroupId;
  ULONG LoaderThreads;
  UNICODE_STRING RedirectionDllName;
  UNICODE_STRING HeapPartitionName;
  ULONG_PTR DefaultThreadpoolCpuSetMasks;
  ULONG DefaultThreadpoolCpuSetMaskCount;
} RTL_USER_PROCESS_PARAMETERS_EXTENDED;

#if !defined(_MSC_VER)
#pragma pack(pop)
#else
#include <poppack.h>
#endif
BOOST_PROCESS_V2_DECL std::wstring cwd_cmd_from_proc(HANDLE proc, int type, error_code & ec);
BOOST_PROCESS_V2_DECL HANDLE open_process_with_debug_privilege(boost::process::v2::pid_type pid, error_code & ec);
#endif

} // namespace ext

} // namespace detail

BOOST_PROCESS_V2_END_NAMESPACE

#endif // BOOST_PROCESS_V2_DETAIL_PROC_INFO_HPP

