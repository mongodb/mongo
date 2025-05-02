// Copyright (c) 2022 Klemens D. Morgenstern
// Copyright (c) 2022 Samuel Venable
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/detail/last_error.hpp>
#include <boost/process/v2/detail/throw_error.hpp>
#include <boost/process/v2/pid.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

#if (defined(__APPLE__) && defined(__MACH__))
#include <TargetConditionals.h>
#if !TARGET_OS_IOS
  #include <sys/proc_info.h>
  #include <libproc.h>
#endif
#endif

#if (defined(__linux__) || defined(__ANDROID__))
#include <dirent.h>
#endif

#if defined(__FreeBSD__)
#include <fcntl.h>
#include <kvm.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

#if (defined(__DragonFly__) ||  defined(__OpenBSD__))
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
#include <fcntl.h>
#include <sys/types.h>
#include <kvm.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/proc.h>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

#if defined(BOOST_PROCESS_V2_WINDOWS)
pid_type current_pid() {return ::GetCurrentProcessId();}
#else
pid_type current_pid() {return ::getpid();}
#endif

#if defined(BOOST_PROCESS_V2_WINDOWS)

std::vector<pid_type> all_pids(error_code & ec)
{
    std::vector<pid_type> vec;
    HANDLE hp = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (!hp)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    }
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(hp, &pe)) 
    {
        do 
        {
            vec.push_back(pe.th32ProcessID);
        } while (Process32Next(hp, &pe));
    }
    CloseHandle(hp);
    return vec;
}

pid_type parent_pid(pid_type pid, error_code & ec)
{
    pid_type ppid = static_cast<pid_type>(-1);
    HANDLE hp = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (!hp)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return ppid;
    }
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(hp, &pe))
    {
        do
        {
            if (pe.th32ProcessID == pid)
            {
                ppid = pe.th32ParentProcessID;
                break;
            }
        }
        while (Process32Next(hp, &pe));
    }
    CloseHandle(hp);
    return ppid;
}

std::vector<pid_type> child_pids(pid_type pid, error_code & ec)
{
    std::vector<pid_type> vec;
    HANDLE hp = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (!hp)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    }
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(hp, &pe))
    {
        do
        {
            if (pe.th32ParentProcessID == pid)
            {
                vec.push_back(pe.th32ProcessID);
            }
        } 
        while (Process32Next(hp, &pe));
    }
    CloseHandle(hp);
    return vec;
}

#elif (defined(__APPLE__) && defined(__MACH__)) && !TARGET_OS_IOS

std::vector<pid_type> all_pids(error_code & ec)
{
    std::vector<pid_type> vec;
    vec.resize(proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0) / sizeof(pid_type));
    const auto sz = proc_listpids(PROC_ALL_PIDS, 0, &vec[0], sizeof(pid_type) * vec.size());
    if (sz < 0)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return {};
    }
    vec.resize(sz);
    return vec;
}

pid_type parent_pid(pid_type pid, error_code & ec)
{
    pid_type ppid = static_cast<pid_type>(-1);
    proc_bsdinfo proc_info;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &proc_info, sizeof(proc_info)) <= 0)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return ppid;
    }
    else
        ppid = proc_info.pbi_ppid;
    return ppid;
}

std::vector<pid_type> child_pids(pid_type pid, error_code & ec)
{
    std::vector<pid_type> vec;
    vec.resize(proc_listpids(PROC_PPID_ONLY, (uint32_t)pid, nullptr, 0) / sizeof(pid_type));
    const auto sz = proc_listpids(PROC_PPID_ONLY, (uint32_t)pid, &vec[0], sizeof(pid_type) * vec.size());
    if (sz < 0)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return {};
    }
    vec.resize(sz);
    return vec;
}

#elif (defined(__linux__) || defined(__ANDROID__))

std::vector<pid_type> all_pids(error_code & ec)
{
    std::vector<pid_type> vec;
    DIR *proc = opendir("/proc");
    if (!proc)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    } 
    struct dirent *ent = nullptr;
    while ((ent = readdir(proc))) 
    {
        if (!isdigit(*ent->d_name))
            continue;
        vec.push_back(atoi(ent->d_name));
    }
    closedir(proc);
    return vec;
}

pid_type parent_pid(pid_type pid, error_code & ec)
{
    pid_type ppid = static_cast<pid_type>(-1);
    char buffer[BUFSIZ];
    sprintf(buffer, "/proc/%d/stat", pid);
    FILE *stat = fopen(buffer, "r");
    if (!stat)
    {
        if (errno == ENOENT)
            BOOST_PROCESS_V2_ASSIGN_EC(ec, ESRCH, system_category());
        else
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return ppid;
    } 
    else
    {
        std::size_t size = fread(buffer, sizeof(char), sizeof(buffer), stat);
        if (size > 0) 
        {
            char *token = nullptr;
            if ((token = strtok(buffer, " "))) 
            {
                if ((token = strtok(nullptr, " "))) 
                {
                    if ((token = strtok(nullptr, " "))) 
                    {
                        if ((token = strtok(nullptr, " "))) 
                        {
                            ppid = (pid_type)strtoul(token, nullptr, 10);
                        }
                    }
                }
            }
            if (!token)
            {
                fclose(stat);
                BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
                return ppid;
            }
        }
        fclose(stat);
    }
    return ppid;
}

std::vector<pid_type> child_pids(pid_type pid, error_code & ec)
{
    std::vector<pid_type> vec;
    std::vector<pid_type> pids = all_pids(ec);
    if (!pids.empty()) 
        vec.reserve(pids.size());
    for (std::size_t i = 0; i < pids.size(); i++)
    {
        pid_type ppid = parent_pid(pids[i], ec);
        if (ppid != -1 && ppid == pid)
            vec.push_back(pids[i]);
        else if (ec.value() == ESRCH)
           ec.clear();
    }
    return vec;
}

#elif defined(__FreeBSD__)

std::vector<pid_type> all_pids(error_code & ec)
{
    std::vector<pid_type> vec;
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
    if (!kd) 
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    }
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_ALL, 0, &cntp))) 
    {
        vec.reserve(cntp);
        for (int i = 0; i < cntp; i++) 
            vec.push_back(proc_info[i].ki_pid);
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return vec;
}

pid_type parent_pid(pid_type pid, error_code & ec)
{
    pid_type ppid = static_cast<pid_type>(-1);
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
    if (!kd) 
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return ppid;
    }
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_PID, pid, &cntp)))
    {
        ppid = proc_info->ki_ppid;
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return ppid;
}

std::vector<pid_type> child_pids(pid_type pid, error_code & ec)
{
    std::vector<pid_type> vec;
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
    if (!kd) 
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    }
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_ALL, 0, &cntp)))
    {
        vec.reserve(cntp);
        for (int i = 0; i < cntp; i++)
        {
            if (proc_info[i].ki_ppid == pid)
            {
                vec.push_back(proc_info[i].ki_pid);
            }
        }
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return vec;
}

#elif defined(__DragonFly__)

std::vector<pid_type> all_pids(error_code & ec)
{
    std::vector<pid_type> vec;
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
    if (!kd) 
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    }
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_ALL, 0, &cntp))) 
    {
        vec.reserve(cntp);
        for (int i = 0; i < cntp; i++) 
            if (proc_info[i].kp_pid >= 0) 
                vec.push_back(proc_info[i].kp_pid);
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return vec;
}

pid_type parent_pid(pid_type pid, error_code & ec)
{
    pid_type ppid = static_cast<pid_type>(-1);
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
    if (!kd) 
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return ppid;
    }
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_PID, pid, &cntp)))
    {
        if (proc_info->kp_ppid >= 0)
        {
            ppid = proc_info->kp_ppid;
        }
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return ppid;
}

std::vector<pid_type> child_pids(pid_type pid, error_code & ec)
{
    std::vector<pid_type> vec;
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
    if (!kd) 
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    }
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_ALL, 0, &cntp)))
    {
        vec.reserve(cntp);
        for (int i = 0; i < cntp; i++)
        {
            if (proc_info[i].kp_pid >= 0 && proc_info[i].kp_ppid >= 0 && proc_info[i].kp_ppid == pid)
            {
                vec.push_back(proc_info[i].kp_pid);
            }
        }
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return vec;
}

#elif defined(__NetBSD__)

std::vector<pid_type> all_pids(error_code & ec)
{
    std::vector<pid_type> vec;
    int cntp = 0;
    kinfo_proc2  *proc_info = nullptr;
  
    struct closer
    {
        void operator()(kvm_t * kd)
        {
            kvm_close(kd);
        }
    };

    std::unique_ptr<kvm_t, closer> kd{kvm_openfiles(nullptr, nullptr, nullptr, KVM_NO_FILES, nullptr)};
    if (!kd)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    } 
    if ((proc_info = kvm_getproc2(kd.get(), KERN_PROC_ALL, 0, sizeof(struct kinfo_proc2), &cntp)))
    {
        vec.reserve(cntp);
        for (int i = cntp - 1; i >= 0; i--) 
        {
            vec.push_back(proc_info[i].p_pid);
        }
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return vec;
}

pid_type parent_pid(pid_type pid, error_code & ec)
{
    pid_type ppid = static_cast<pid_type>(-1);
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
    if (!kd) 
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return ppid;
    }
    if ((proc_info = kvm_getproc2(kd.get(), KERN_PROC_PID, pid, sizeof(struct kinfo_proc2), &cntp)))
    {
        ppid = proc_info->p_ppid;
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return ppid;
}

std::vector<pid_type> child_pids(pid_type pid, error_code & ec)
{
    std::vector<pid_type> vec;
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
    if (!kd) 
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    }
    if ((proc_info = kvm_getproc2(kd.get(), KERN_PROC_ALL, 0, sizeof(struct kinfo_proc2), &cntp)))
    {
        vec.reserve(cntp);
        for (int i = cntp - 1; i >= 0; i--)
        {
            if (proc_info[i].p_ppid == pid)
            {
                vec.push_back(proc_info[i].p_pid);
            }
        }
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return vec;
}

#elif defined(__OpenBSD__)

std::vector<pid_type> all_pids(error_code & ec)
{
    std::vector<pid_type> vec;
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
    if (!kd)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    } 
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_ALL, 0, sizeof(struct kinfo_proc), &cntp)))
    {
        vec.reserve(cntp);
        for (int i = cntp - 1; i >= 0; i--)
        {
            if (proc_info[i].p_pid >= 0)
            {
                vec.push_back(proc_info[i].p_pid);
            }
        }
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return vec;
}

pid_type parent_pid(pid_type pid, error_code & ec)
{
    pid_type ppid = static_cast<pid_type>(-1);
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
    if (!kd)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return ppid;
    }
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_PID, pid, sizeof(struct kinfo_proc), &cntp)))
    {
        ppid = proc_info->p_ppid;
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return ppid;
}

std::vector<pid_type> child_pids(pid_type pid, error_code & ec)
{
    std::vector<pid_type> vec;
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
    if (!kd) 
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    }
    if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_ALL, 0, sizeof(struct kinfo_proc), &cntp)))
    {
        vec.reserve(cntp);
        for (int i = cntp - 1; i >= 0; i--)
        {
            if (proc_info[i].p_ppid == pid)
            {
                vec.push_back(proc_info[i].p_pid);
            }
        }
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return vec;
}


#elif defined(__sun)

std::vector<pid_type> all_pids(error_code & ec)
{
    std::vector<pid_type> vec;
    struct pid cur_pid;
    proc *proc_info = nullptr;
  
    struct closer
    {
        void operator()(kvm_t * kd)
        {
            kvm_close(kd);
        }
    };

    std::unique_ptr<kvm_t, closer> kd{kvm_open(nullptr, nullptr, nullptr, O_RDONLY, nullptr)};
    if (!kd)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    } 
    while ((proc_info = kvm_nextproc(kd.get())))
    {
        if (kvm_kread(kd.get(), (std::uintptr_t)proc_info->p_pidp, &cur_pid, sizeof(cur_pid)) != -1)
        {
            vec.insert(vec.begin(), cur_pid.pid_id);
        }
        else
        {
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
            break;
        }
    }
    return vec;
}

pid_type parent_pid(pid_type pid, error_code & ec)
{
    pid_type ppid = static_cast<pid_type>(-1);
    proc *proc_info = nullptr;
  
    struct closer
    {
        void operator()(kvm_t * kd)
        {
            kvm_close(kd);
        }
    };

    std::unique_ptr<kvm_t, closer> kd{kvm_open(nullptr, nullptr, nullptr, O_RDONLY, nullptr)};
    if (!kd)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return ppid;
    }
    if ((proc_info = kvm_getproc(kd.get(), pid)))
    {
        ppid = proc_info->p_ppid;
    }
    else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    return ppid;
}

std::vector<pid_type> child_pids(pid_type pid, error_code & ec)
{
    std::vector<pid_type> vec;
    struct pid cur_pid;
    proc *proc_info = nullptr;

    struct closer
    {
        void operator()(kvm_t * kd)
        {
            kvm_close(kd);
        }
    };

    std::unique_ptr<kvm_t, closer> kd{kvm_open(nullptr, nullptr, nullptr, O_RDONLY, nullptr)};
    if (!kd)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return vec;
    }
    while ((proc_info = kvm_nextproc(kd.get())))
    {
        if (proc_info->p_ppid == pid)
        {
            if (kvm_kread(kd.get(), (std::uintptr_t)proc_info->p_pidp, &cur_pid, sizeof(cur_pid)) != -1)
            {
                vec.insert(vec.begin(), cur_pid.pid_id);
            }
            else
            {
                BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
                break;
            }
        }
    }
    return vec;
}

#else
std::vector<pid_type> all_pids(error_code & ec)
{
  BOOST_PROCESS_V2_ASSIGN_EC(ec, ENOTSUP, system_category());
  return {};
}
pid_type parent_pid(pid_type pid, error_code & ec)
{
  BOOST_PROCESS_V2_ASSIGN_EC(ec, ENOTSUP, system_category());
  return pid;
}
std::vector<pid_type> child_pids(pid_type, error_code & ec)
{
  BOOST_PROCESS_V2_ASSIGN_EC(ec, ENOTSUP, system_category());
  return {};
}
#endif

std::vector<pid_type> all_pids()
{
    error_code ec;
    auto res = all_pids(ec);
    if (ec)
        detail::throw_error(ec, "all_pids");
    return res;
}

pid_type parent_pid(pid_type pid)
{
    error_code ec;
    auto res = parent_pid(pid, ec);
    if (ec)
        detail::throw_error(ec, "parent_pid");
    return res;
}

std::vector<pid_type> child_pids(pid_type pid)
{
    error_code ec;
    auto res = child_pids(pid, ec);
    if (ec)
        detail::throw_error(ec, "child_pids");
    return res;
}

BOOST_PROCESS_V2_END_NAMESPACE
