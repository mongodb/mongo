// Copyright (c) 2022 Klemens D. Morgenstern
// Copyright (c) 2022 Samuel Venable
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/process/v2/detail/config.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <boost/asio/windows/basic_object_handle.hpp>
#endif

#include <boost/process/v2/detail/last_error.hpp>
#include <boost/process/v2/detail/throw_error.hpp>
#include <boost/process/v2/ext/detail/proc_info.hpp>

#include <string>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace detail
{

namespace ext
{

#if defined(BOOST_PROCESS_V2_WINDOWS)
// type of process memory to read?
enum MEMTYP {MEMCMD, MEMCWD};
std::wstring cwd_cmd_from_proc(HANDLE proc, int type, error_code & ec)
{
    std::wstring buffer;
    PEB peb;
    SIZE_T nRead = 0; 
    ULONG len = 0;
    PROCESS_BASIC_INFORMATION pbi;
    RTL_USER_PROCESS_PARAMETERS_EXTENDED upp;

    NTSTATUS status = 0;
    PVOID buf = nullptr;
    status = NtQueryInformationProcess(proc, ProcessBasicInformation, &pbi, sizeof(pbi), &len);
    ULONG error = RtlNtStatusToDosError(status);

    if (error)
    {
        BOOST_PROCESS_V2_ASSIGN_EC(ec, error, system_category());
        return {};
    }

    if (!ReadProcessMemory(proc, pbi.PebBaseAddress, &peb, sizeof(peb), &nRead))
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return {};
    }

    if (!ReadProcessMemory(proc, peb.ProcessParameters, &upp, sizeof(upp), &nRead))
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return {};
    }

    if (type == MEMCWD)
    {
        buf = upp.CurrentDirectory.DosPath.Buffer;
        len = upp.CurrentDirectory.DosPath.Length;
    }
    else if (type == MEMCMD)
    {
        buf = upp.CommandLine.Buffer;
        len = upp.CommandLine.Length;
    }

    buffer.resize(len / 2 + 1);

    if (!ReadProcessMemory(proc, buf, &buffer[0], len, &nRead))
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return {};
    }

    buffer.pop_back();
    return buffer;
}

// with debug_privilege enabled allows reading info from more processes
// this includes stuff such as exe path, cwd path, cmdline, and environ
HANDLE open_process_with_debug_privilege(boost::process::v2::pid_type pid, error_code & ec)
{
    HANDLE proc = nullptr;
    HANDLE hToken = nullptr;
    LUID luid;
    TOKEN_PRIVILEGES tkp;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        if (LookupPrivilegeValue(nullptr, SE_DEBUG_NAME, &luid))
        {
            tkp.PrivilegeCount = 1;
            tkp.Privileges[0].Luid = luid;
            tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (AdjustTokenPrivileges(hToken, false, &tkp, sizeof(tkp), nullptr, nullptr))
            {
                proc = OpenProcess(PROCESS_ALL_ACCESS, false, pid);
            }
        }
        CloseHandle(hToken);
    }
    if (!proc)
        proc = OpenProcess(PROCESS_ALL_ACCESS, false, pid);
    if (!proc)
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return proc;
}
#endif

} // namespace ext

} // namespace detail

BOOST_PROCESS_V2_END_NAMESPACE
