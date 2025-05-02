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
#include <boost/process/v2/ext/exe.hpp>

#include <string>
#include <vector>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <windows.h>
#else
#include <climits>
#endif

#if (defined(__APPLE__) && defined(__MACH__))
#include <TargetConditionals.h>
#if !TARGET_OS_IOS
  #include <sys/proc_info.h>
  #include <libproc.h>
#endif
#endif

#if (defined(BOOST_PROCESS_V2_WINDOWS) || defined(__linux__) || defined(__ANDROID__) || defined(__sun))
#include <cstdlib>
#endif

#if (defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__))
#include <sys/types.h>
#include <sys/sysctl.h>
#if !defined(__FreeBSD__) && !defined(__NetBSD__)
#include <alloca.h>
#endif
#endif

#if defined(__OpenBSD__)
#include <boost/process/v2/ext/cwd.hpp>
#include <boost/process/v2/ext/cmd.hpp>
#include <boost/process/v2/ext/env.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <kvm.h>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace ext {

#if defined(BOOST_PROCESS_V2_WINDOWS)

filesystem::path exe(HANDLE process_handle)
{
    error_code ec;
    auto res = exe(process_handle, ec);
    if (ec)
        detail::throw_error(ec, "exe");
    return res;
}


filesystem::path exe(HANDLE proc, error_code & ec)
{
#if _WIN32_WINNT >= 0x0600
    wchar_t buffer[MAX_PATH];
    // On input, specifies the size of the lpExeName buffer, in characters.
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(proc, 0, buffer, &size))
    {
        return filesystem::canonical(buffer, ec);
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
#else
    BOOST_PROCESS_V2_ASSIGN_EC(ec, net::error::operation_not_supported);
#endif
    return "";
}

filesystem::path exe(boost::process::v2::pid_type pid, error_code & ec)
{
    if (pid == GetCurrentProcessId())
    {
        wchar_t buffer[MAX_PATH];
        if (GetModuleFileNameW(nullptr, buffer, sizeof(buffer))) 
        {
            return filesystem::canonical(buffer, ec);
        }
    } 
    else 
    {
        struct del
        {
            void operator()(HANDLE h)
            {
                ::CloseHandle(h);
            };
        };
        std::unique_ptr<void, del> proc{detail::ext::open_process_with_debug_privilege(pid, ec)};
        if (proc == nullptr)
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        else
            return exe(proc.get(), ec);
    }
    return "";
}

#elif (defined(__APPLE__) && defined(__MACH__)) && !TARGET_OS_IOS

filesystem::path exe(boost::process::v2::pid_type pid, error_code & ec)
{
    char buffer[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, buffer, sizeof(buffer)) > 0) 
    {
        return filesystem::canonical(buffer, ec);
    }
    BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return "";
}

#elif (defined(__linux__) || defined(__ANDROID__) || defined(__sun))

filesystem::path exe(boost::process::v2::pid_type pid, error_code & ec)
{
#if (defined(__linux__) || defined(__ANDROID__))
    return filesystem::canonical(
            filesystem::path("/proc") / std::to_string(pid) / "exe", ec
            );
#elif defined(__sun)
    return filesystem::canonical(
            filesystem::path("/proc") / std::to_string(pid) / "path/a.out", ec
            );
#endif
}

#elif (defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__))

filesystem::path exe(boost::process::v2::pid_type pid, error_code & ec)
{
#if (defined(__FreeBSD__) || defined(__DragonFly__))
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, pid};
#elif defined(__NetBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC_ARGS, pid, KERN_PROC_PATHNAME};
#endif
    std::size_t len = 0;
    if (sysctl(mib, 4, nullptr, &len, nullptr, 0) == 0) 
    {
        std::string strbuff;
        strbuff.resize(len);
        if (sysctl(mib, 4, &strbuff[0], &len, nullptr, 0) == 0)
        {
            strbuff.resize(len - 1);
            return filesystem::canonical(strbuff, ec);
        }
    }

    BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return "";
}

#elif defined(__OpenBSD__)

filesystem::path exe(boost::process::v2::pid_type pid, error_code & ec)
{
    BOOST_PROCESS_V2_ASSIGN_EC(ec, ENOTSUP, system_category());
    return "";
}

#else
filesystem::path exe(boost::process::v2::pid_type pid, error_code & ec)
{
  BOOST_PROCESS_V2_ASSIGN_EC(ec, ENOTSUP, system_category());
  return "";
}
#endif

filesystem::path exe(boost::process::v2::pid_type pid)
{
    error_code ec;
    auto res = exe(pid, ec);
    if (ec)
        detail::throw_error(ec, "exe");
    return res;
}


} // namespace ext

BOOST_PROCESS_V2_END_NAMESPACE
