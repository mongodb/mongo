// @file hook_windows_memory.cpp : Used to hook Windows functions that allocate virtual memory

/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#if defined(_WIN32)

#include "mongo/util/hook_windows_memory.h"

#include <vector>

#include "mongo/platform/windows_basic.h"
#include "mongo/util/concurrency/remap_lock.h"
#include "mongo/util/hook_win32.h"

namespace {

    // hooked function typedefs and routines

    typedef PVOID ( WINAPI *RtlCreateHeap_t ) ( ULONG, PVOID, SIZE_T, SIZE_T, PVOID, void* );

    RtlCreateHeap_t originalRtlCreateHeap_kernel32;

    static PVOID WINAPI myRtlCreateHeap_kernel32(
        ULONG Flags,
        PVOID HeapBase,
        SIZE_T ReserveSize,
        SIZE_T CommitSize,
        PVOID Lock,
        void* /* PRTL_HEAP_PARAMETERS */ Parameters
    ) {
        mongo::RemapLock remapLock;
        return originalRtlCreateHeap_kernel32( Flags,
                                               HeapBase,
                                               ReserveSize,
                                               CommitSize,
                                               Lock,
                                               Parameters );
    }

    RtlCreateHeap_t originalRtlCreateHeap_kernelbase;

    static PVOID WINAPI myRtlCreateHeap_kernelbase(
        ULONG Flags,
        PVOID HeapBase,
        SIZE_T ReserveSize,
        SIZE_T CommitSize,
        PVOID Lock,
        void* /* PRTL_HEAP_PARAMETERS */ Parameters
    ) {
        mongo::RemapLock remapLock;
        return originalRtlCreateHeap_kernelbase( Flags,
                                                 HeapBase,
                                                 ReserveSize,
                                                 CommitSize,
                                                 Lock,
                                                 Parameters );
    }

    typedef LONG ( WINAPI *NtAllocateVirtualMemory_t )
            ( HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG );

    NtAllocateVirtualMemory_t originalNtAllocateVirtualMemory_kernel32;

    static LONG WINAPI myNtAllocateVirtualMemory_kernel32(
        HANDLE ProcessHandle,
        PVOID* BaseAddress,
        ULONG_PTR ZeroBits,
        PSIZE_T RegionSize,
        ULONG AllocationType,
        ULONG Protect
    ) {
        mongo::RemapLock remapLock;
        return originalNtAllocateVirtualMemory_kernel32( ProcessHandle,
                                                         BaseAddress,
                                                         ZeroBits,
                                                         RegionSize,
                                                         AllocationType,
                                                         Protect );
    }

    NtAllocateVirtualMemory_t originalNtAllocateVirtualMemory_kernelbase;

    static LONG WINAPI myNtAllocateVirtualMemory_kernelbase(
        HANDLE ProcessHandle,
        PVOID* BaseAddress,
        ULONG_PTR ZeroBits,
        PSIZE_T RegionSize,
        ULONG AllocationType,
        ULONG Protect
    ) {
        mongo::RemapLock remapLock;
        return originalNtAllocateVirtualMemory_kernelbase( ProcessHandle,
                                                           BaseAddress,
                                                           ZeroBits,
                                                           RegionSize,
                                                           AllocationType,
                                                           Protect );
    }

} // namespace

namespace mongo {

    void hookWindowsMemory( void ) {

        // kernel32.dll calls directly prior to Windows 7
        char* hookModuleAddress = reinterpret_cast<char*>( GetModuleHandleA( "kernel32.dll" ) );
        originalRtlCreateHeap_kernel32 = reinterpret_cast<RtlCreateHeap_t>(
                        hookWin32( hookModuleAddress,
                                   "ntdll.dll",
                                   "RtlCreateHeap",
                                   myRtlCreateHeap_kernel32 ) );
        originalNtAllocateVirtualMemory_kernel32 = reinterpret_cast<NtAllocateVirtualMemory_t>(
                        hookWin32( hookModuleAddress,
                                   "ntdll.dll",
                                   "NtAllocateVirtualMemory",
                                    myNtAllocateVirtualMemory_kernel32 ) );

        // in Windows 7 and Server 2008 R2, calls are through kernelbase.dll
        hookModuleAddress = reinterpret_cast<char*>( GetModuleHandleA( "kernelbase.dll" ) );
        originalRtlCreateHeap_kernelbase = reinterpret_cast<RtlCreateHeap_t>(
                hookWin32( hookModuleAddress,
                           "ntdll.dll",
                           "RtlCreateHeap",
                           myRtlCreateHeap_kernelbase ) );
        originalNtAllocateVirtualMemory_kernelbase = reinterpret_cast<NtAllocateVirtualMemory_t>(
                hookWin32( hookModuleAddress,
                           "ntdll.dll",
                           "NtAllocateVirtualMemory",
                           myNtAllocateVirtualMemory_kernelbase ) );
    }

} //namespace mongo

#endif // #if defined(_WIN32)
