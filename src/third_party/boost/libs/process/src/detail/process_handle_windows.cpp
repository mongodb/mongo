// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/process/v2/detail/config.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)

#include <boost/process/v2/detail/last_error.hpp>
#include <boost/process/v2/detail/throw_error.hpp>
#include <boost/process/v2/detail/process_handle_windows.hpp>
#include <boost/process/v2/ext/detail/proc_info.hpp>

#include <windows.h>

#if !defined(BOOST_PROCESS_V2_DISABLE_UNDOCUMENTED_API)
extern "C" 
{

LONG WINAPI NtResumeProcess(HANDLE ProcessHandle);
LONG WINAPI NtSuspendProcess(HANDLE ProcessHandle);

}
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace detail
{

void get_exit_code_(
    HANDLE handle,
    native_exit_code_type & exit_code, 
    error_code & ec)
{
    if (!::GetExitCodeProcess(handle, &exit_code))
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
}


HANDLE open_process_(DWORD pid)
{
    auto proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (proc == nullptr)
    {
        error_code ec;
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        throw system_error(ec, "open_process()");
    }

    return proc;
}


void terminate_if_running_(HANDLE handle)
{
    DWORD exit_code = 0u;
    if (handle == INVALID_HANDLE_VALUE)
        return ;
    if (::GetExitCodeProcess(handle, &exit_code))
        if (exit_code == STILL_ACTIVE)
            ::TerminateProcess(handle, 260);
}

bool check_handle_(HANDLE handle, error_code & ec)
{
    if (handle == INVALID_HANDLE_VALUE)
    {
        BOOST_PROCESS_V2_ASSIGN_EC(ec, ERROR_INVALID_HANDLE_STATE, system_category());
        return false;
    }
    return true;
}

bool check_pid_(pid_type pid_, error_code & ec)
{
    if (pid_ == 0)
    {
        BOOST_PROCESS_V2_ASSIGN_EC(ec, ERROR_INVALID_HANDLE_STATE, system_category());
        return false;
    }
    return true;
}

struct enum_windows_data_t
{
    error_code &ec;
    pid_type pid;
};

static BOOL CALLBACK enum_window(HWND hwnd, LPARAM param)
{
    auto data = reinterpret_cast<enum_windows_data_t*>(param);
    DWORD pid{0u};
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != data->pid)
        return TRUE;
    
    LRESULT res = ::SendMessageW(hwnd, WM_CLOSE, 0, 0);

    if (res)
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(data->ec);
    return res == 0;
}

void request_exit_(pid_type pid_, error_code & ec)
{
    enum_windows_data_t data{ec, pid_};

    if (!::EnumWindows(enum_window, reinterpret_cast<LONG_PTR>(&data)))
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
}

void interrupt_(pid_type pid_, error_code & ec)
{
    if (!::GenerateConsoleCtrlEvent(CTRL_C_EVENT, pid_))
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
}

void terminate_(HANDLE handle, error_code & ec, DWORD & exit_status)
{
    if (!::TerminateProcess(handle, 260))
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
}

void check_running_(HANDLE handle, error_code & ec, DWORD & exit_status)
{
    if (!::GetExitCodeProcess(handle, &exit_status))
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
}

#if !defined(BOOST_PROCESS_V2_DISABLE_UNDOCUMENTED_API)
void suspend_(HANDLE handle, error_code & ec)
{
    auto nt_err = NtSuspendProcess(handle);
    ULONG dos_err = RtlNtStatusToDosError(nt_err);
    if (dos_err)
       BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
}

void resume_(HANDLE handle, error_code & ec)
{
    auto nt_err = NtResumeProcess(handle);
    ULONG dos_err = RtlNtStatusToDosError(nt_err);
    if (dos_err)
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
}
#else
void suspend_(HANDLE, error_code & ec)
{
    BOOST_PROCESS_V2_ASSIGN_EC(ec, ERROR_CALL_NOT_IMPLEMENTED, system_category());
}

void resume_(HANDLE handle, error_code & ec)
{
    BOOST_PROCESS_V2_ASSIGN_EC(ec, ERROR_CALL_NOT_IMPLEMENTED, system_category());
}
#endif


}


BOOST_PROCESS_V2_END_NAMESPACE

#endif
