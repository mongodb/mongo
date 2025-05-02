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
#include <boost/process/v2/ext/cmd.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#endif

#if (defined(__APPLE__) && defined(__MACH__))
#include <TargetConditionals.h>
#if !TARGET_OS_IOS
  #include <sys/proc_info.h>
  #include <sys/sysctl.h>
  #include <libproc.h>
#endif
#endif

#if (defined(__linux__) || defined(__ANDROID__))
#include <cstdio>
#endif

#if (defined(__FreeBSD__) || defined(__DragonFly__) ||  defined(__OpenBSD__))
#include <fcntl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <kvm.h>
#endif

#if defined(__NetBSD__)
#include <sys/types.h>
#include <kvm.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

#if defined(__sun)
#include <cstdlib>
#include <sys/types.h>
#include <kvm.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/proc.h>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

struct make_cmd_shell_
{
#if defined(BOOST_PROCESS_V2_WINDOWS)
    static shell make(std::wstring data)
    {
        shell res;
        res.input_  = res.buffer_ = std::move(data);
        res.parse_();
        return res;
    }
#else
    static shell make(std::string data,
                      int argc, char ** argv,
                      void(*free_func)(int, char**))
    {
        shell res;
        res.argc_  = argc;
        res.input_ = res.buffer_ = std::move(data);
        res.argv_  = argv;
        res.free_argv_ = free_func;

        return res;
    }

    static shell clone(char ** cmd)
    {
        shell res;
        res.argc_ = 0;
        std::size_t str_lengths = 0;
        for (auto c = cmd; *c != nullptr; c++)
        {
            res.argc_++;
            str_lengths += (std::strlen(*c) + 1);
        }
        // yes, not the greatest solution.
        res.buffer_.resize(str_lengths);

        res.argv_ = new char*[res.argc_ + 1];
        res.free_argv_ = +[](int /*argc*/, char ** argv) {delete[] argv;};
        res.argv_[res.argc_] = nullptr;
        auto p = &*res.buffer_.begin();

        for (int i = 0; i < res.argc_; i++)
        {
            const auto ln = std::strlen(cmd[i]);
            res.argv_[i] = std::strcpy(p, cmd[i]);
            p += (ln + 1);
        }

        return res;
    }

#endif
};

namespace ext {

#if defined(BOOST_PROCESS_V2_WINDOWS)

shell cmd(HANDLE proc, error_code & ec)
{
    std::wstring buffer = boost::process::v2::detail::ext::cwd_cmd_from_proc(proc, 0/*=MEMCMD*/, ec);

    if (!ec)
        return make_cmd_shell_::make(std::move(buffer));
    else
        return {};
}

shell cmd(HANDLE proc)
{
    error_code ec;
    auto res = cmd(proc, ec);
    if (ec)
        detail::throw_error(ec, "cmd");
    return res;
}

shell cmd(boost::process::v2::pid_type pid, error_code & ec)
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
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return shell{};
    }
    else
        return cmd(proc.get(), ec);

}
    
#elif (defined(__APPLE__) && defined(__MACH__)) && !TARGET_OS_IOS

shell cmd(boost::process::v2::pid_type pid, error_code & ec)
{
    int mib[3] = {CTL_KERN, KERN_ARGMAX, 0};
    int argmax = 0;
    auto size = sizeof(argmax);
    if (sysctl(mib, 2, &argmax, &size, nullptr, 0) == -1)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return {};
    }

    std::string procargs;
    procargs.resize(argmax - 1);
    mib[1] = KERN_PROCARGS;
    mib[2] = pid;

    size = argmax;

    if (sysctl(mib, 3, &*procargs.begin(), &size, nullptr, 0) != 0)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return {};
    }

    int argc = *reinterpret_cast<const int*>(procargs.data());
    auto itr = procargs.begin() + sizeof(argc);

    std::unique_ptr<char*[]> argv{new char*[argc + 1]};
    const auto end = procargs.end();

    argv[argc] = nullptr; //args is a null-terminated list

    for (auto n = 0u; n <= argc; n++)
    {
        auto e = std::find(itr, end, '\0');
        if (e == end && n < argc) // something off
        {
            BOOST_PROCESS_V2_ASSIGN_EC(ec, EINVAL, system_category());
            return {};
        }
        argv[n] = &*itr;
        itr = e + 1; // start searching start
    }

    auto fr_func = +[](int argc, char ** argv) {delete [] argv;};

    return make_cmd_shell_::make(std::move(procargs), argc, argv.release(), fr_func);
}
    
#elif (defined(__linux__) || defined(__ANDROID__))

shell cmd(boost::process::v2::pid_type pid, error_code & ec)
{
    std::string procargs;
    procargs.resize(4096);
    int f = ::open(("/proc/" + std::to_string(pid) + "/cmdline").c_str(), O_RDONLY);

    while (procargs.back() != EOF)
    {
        auto r = ::read(f, &*(procargs.end() - 4096), 4096);
        if (r < 0)
        {
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
            ::close(f);
            return {};
        }
        if (r < 4096) // done!
        {
            procargs.resize(procargs.size() - 4096 + r);
            break;
        }
        procargs.resize(procargs.size() + 4096);
    }
    ::close(f);

    if (procargs.back() == EOF)
        procargs.pop_back();

    auto argc = std::count(procargs.begin(), procargs.end(), '\0');

    auto itr = procargs.begin();

    std::unique_ptr<char*[]> argv{new char*[argc + 1]};
    const auto end = procargs.end();

    argv[argc] = nullptr; //args is a null-terminated list


    for (auto n = 0u; n <= argc; n++)
    {
        auto e = std::find(itr, end, '\0');
        if (e == end && n < argc) // something off
        {
            BOOST_PROCESS_V2_ASSIGN_EC(ec, EINVAL, system_category());
            return {};
        }
        argv[n] = &*itr;
        itr = e + 1; // start searching start
    }

    auto fr_func = +[](int /*argc*/, char ** argv) {delete [] argv;};

    return make_cmd_shell_::make(std::move(procargs), argc, argv.release(), fr_func);
}
    
#elif (defined(__FreeBSD__) || defined(__DragonFly__))

shell cmd(boost::process::v2::pid_type pid, error_code & ec)
{
    int cntp = 0;
    kinfo_proc *proc_info = nullptr;
    const char *nlistf, *memf;
    nlistf = memf = "/dev/null";
  
    struct closer
    {
        void operator()(kvm_t * kd)
        {
            kvm_close(kd);
        }
    };

    std::unique_ptr<kvm_t, closer> kd{kvm_openfiles(nlistf, memf, nullptr, O_RDONLY, nullptr)};
    if (!kd) {BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec); return {};}
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_PID, pid, &cntp))) 
    {
        char **cmd = kvm_getargv(kd.get(), proc_info, 0);
        if (cmd)
            return make_cmd_shell_::clone(cmd);
        else
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return {};
}
    
#elif defined(__NetBSD__)

shell cmd(boost::process::v2::pid_type pid, error_code & ec)
{
    int cntp = 0;
    kinfo_proc2 *proc_info = nullptr;
  
    struct closer
    {
        void operator()(kvm_t * kd)
        {
            kvm_close(kd);
        }
    };

    std::unique_ptr<kvm_t, closer> kd{kvm_openfiles(nullptr, nullptr, nullptr, KVM_NO_FILES, nullptr)};

    if (!kd) {BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec); return {};}
    if ((proc_info = kvm_getproc2(kd.get(), KERN_PROC_PID, pid, sizeof(struct kinfo_proc2), &cntp))) 
    {
        char **cmd = kvm_getargv2(kd.get(), proc_info, 0);
        if (cmd)
            return make_cmd_shell_::clone(cmd);
        else
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return {};
}
    
#elif defined(__OpenBSD__)

shell cmd(boost::process::v2::pid_type pid, error_code & ec)
{
    int cntp = 0;
    kinfo_proc *proc_info = nullptr;

    struct closer
    {
        void operator()(kvm_t * kd)
        {
            kvm_close(kd);
        }
    };

    std::unique_ptr<kvm_t, closer> kd{kvm_openfiles(nullptr, nullptr, nullptr, KVM_NO_FILES, nullptr)};
    if (!kd) {BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec); return {};}
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_PID, pid, sizeof(struct kinfo_proc), &cntp))) 
    {
        char **cmd = kvm_getargv(kd.get(), proc_info, 0);
        if (cmd)
            return make_cmd_shell_::clone(cmd);
        else
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return {};
}
    
#elif defined(__sun)

shell cmd(boost::process::v2::pid_type pid, error_code & ec)
{
    char **cmd = nullptr;
    proc *proc_info = nullptr;
    user *proc_user = nullptr;

    struct closer
    {
        void operator()(kvm_t * kd)
        {
            kvm_close(kd);
        }
    };

    std::unique_ptr<kvm_t, closer> kd{kvm_open(nullptr, nullptr, nullptr, O_RDONLY, nullptr)};
    if (!kd) {BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec); return {};}
    if ((proc_info = kvm_getproc(kd.get(), pid))) 
    {
        if ((proc_user = kvm_getu(kd.get(), proc_info))) 
        {
            if (!kvm_getcmd(kd.get(), proc_info, proc_user, &cmd, nullptr)) 
            {
                int argc = 0;
                for (int i = 0; cmd[i] != nullptr; i++)
                    argc++;
                return make_cmd_shell_::make(
                        {}, argc, cmd,
                        +[](int, char ** argv) {::free(argv);});
            }
            else
                BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        }
        else
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return {};
}

#else
filesystem::path cmd(boost::process::v2::pid_type, error_code & ec)
{
  BOOST_PROCESS_V2_ASSIGN_EC(ec, ENOTSUP, system_category());
  return "";
}
#endif

shell cmd(boost::process::v2::pid_type pid)
{
    error_code ec;
    auto res = cmd(pid, ec);
    if (ec)
            detail::throw_error(ec, "cmd");
    return res;
}

} // namespace ext

BOOST_PROCESS_V2_END_NAMESPACE
