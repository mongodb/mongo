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
#include <boost/process/v2/ext/cwd.hpp>

#include <string>

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

#if defined(__FreeBSD__)
#include <cstring>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

#if (defined(__NetBSD__) || defined(__OpenBSD__))
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#if defined(__DragonFly__)
#include <climits>
#include <cstring>
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#ifdef BOOST_PROCESS_USE_STD_FS
namespace filesystem = std::filesystem;
#else
namespace filesystem = boost::filesystem;
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace ext {

#if defined(BOOST_PROCESS_V2_WINDOWS)

filesystem::path cwd(HANDLE proc, error_code & ec)
{
    auto buffer = boost::process::v2::detail::ext::cwd_cmd_from_proc(proc, 1/*=MEMCWD*/, ec);
    if (!buffer.empty())
      return filesystem::canonical(buffer, ec);
    else 
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return "";
}

filesystem::path cwd(boost::process::v2::pid_type pid, error_code & ec)
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
        return cwd(proc.get(), ec);
    return {};
}

filesystem::path cwd(HANDLE proc)
{
    error_code ec;
    auto res = cwd(proc, ec);
    if (ec)
        detail::throw_error(ec, "cwd");
    return res;
}

#elif (defined(__APPLE__) && defined(__MACH__)) && !TARGET_OS_IOS

filesystem::path cwd(boost::process::v2::pid_type pid, error_code & ec)
{
    proc_vnodepathinfo vpi;
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) > 0)
        return filesystem::canonical(vpi.pvi_cdir.vip_path, ec);
    else
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return "";
}

#elif (defined(__linux__) || defined(__ANDROID__) || defined(__sun))

filesystem::path cwd(boost::process::v2::pid_type pid, error_code & ec)
{
#if (defined(__linux__) || defined(__ANDROID__))
    return filesystem::canonical(
            filesystem::path("/proc") / std::to_string(pid) / "cwd", ec
            );
#elif defined(__sun)
    return filesystem::canonical(
            filesystem::path("/proc") / std::to_string(pid) / "path/cwd", ec
            );
#endif
}

#elif defined(__FreeBSD__)

filesystem::path cwd(boost::process::v2::pid_type pid, error_code & ec)
{
    filesystem::path path;
    struct kinfo_file kif;
    std::size_t sz = 4, len = sizeof(kif);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_CWD, pid};
    if (sysctl(mib, sz, nullptr, &len, nullptr, 0) == 0) 
    {
        memset(&kif, 0, len);
        if (sysctl(mib, sz, &kif, &len, nullptr, 0) == 0) 
        {
            path = filesystem::canonical(kif.kf_path, ec);
        }
        else
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return path;
}

#elif defined(__DragonFly__)

filesystem::path cwd(boost::process::v2::pid_type pid, error_code & ec)
{
    filesystem::path path;
    char buffer[PATH_MAX];
    std::size_t sz = 4, len = sizeof(buffer);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_CWD, pid};
    if (sysctl(mib, sz, buffer, &len, nullptr, 0) == 0)
    {
        path = filesystem::canonical(buffer, ec);
    }    
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return path;
}

#elif (defined(__NetBSD__) || defined(__OpenBSD__))

filesystem::path cwd(boost::process::v2::pid_type pid, error_code & ec)
{
    filesystem::path path;
#if defined(__NetBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC_ARGS, pid, KERN_PROC_CWD};
    const std::size_t sz = 4;
#elif defined(__OpenBSD__)
    int mib[3] = {CTL_KERN, KERN_PROC_CWD, pid};
    const std::size_t sz = 3;
#endif
    std::size_t len = 0;
    if (sysctl(mib, sz, nullptr, &len, nullptr, 0) == 0) 
    {
        std::vector<char> vecbuff;
        vecbuff.resize(len);
        if (sysctl(mib, sz, &vecbuff[0], &len, nullptr, 0) == 0)
        {
            path = filesystem::canonical(&vecbuff[0], ec);
        }
        else
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return path;
}

#else
filesystem::path cwd(boost::process::v2::pid_type pid, error_code & ec)
{
  BOOST_PROCESS_V2_ASSIGN_EC(ec, ENOTSUP, system_category());
  return "";
}
#endif

filesystem::path cwd(boost::process::v2::pid_type pid)
{
    error_code ec;
    auto res = cwd(pid, ec);
    if (ec)
        detail::throw_error(ec, "cwd");
    return res;
}

} // namespace ext

BOOST_PROCESS_V2_END_NAMESPACE
