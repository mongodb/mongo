// Copyright (c) 2022 Klemens D. Morgenstern
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
#include <boost/process/v2/ext/env.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#endif

#if (defined(__APPLE__) && defined(__MACH__))
#include <TargetConditionals.h>
#if !TARGET_OS_IOS
  #include <algorithm>
  #include <sys/proc_info.h>
  #include <sys/sysctl.h>
  #include <libproc.h>
#endif
#endif

#if (defined(__linux__) || defined(__ANDROID__))
#include <cstdio>
#endif

#if defined(__FreeBSD__)
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <kvm.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

#if defined(__DragonFly__)
#include <algorithm>
#include <cstring>
#include <sys/types.h>
#include <kvm.h>
#endif

#if defined(__NetBSD__)
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <kvm.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

#if defined(__OpenBSD__)
#include <algorithm>
#include <cstring>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <kvm.h>
#endif

#if defined(__sun)
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <kvm.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/user.h>
#include <sys/proc.h>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace detail {
namespace ext {

#if defined(BOOST_PROCESS_V2_WINDOWS)

void native_env_handle_deleter::operator()(native_env_handle_type h) const
{
    delete [] h;
}

native_env_iterator next(native_env_iterator nh)
{
    while (*nh != L'\0')
        nh ++;
    return ++nh ;
}
native_env_iterator find_end(native_env_iterator nh)
{
    while (*nh - 1 != L'\0' && *nh != L'\0')
        nh ++;
    return nh ;
}

const environment::char_type * dereference(native_env_iterator iterator)
{
    return iterator;
}

#elif (defined(__linux__) || defined(__ANDROID__))
//linux stores this as a blob with an EOF at the end

void native_env_handle_deleter::operator()(native_env_handle_type h) const
{
    delete [] h;
}

native_env_iterator next(native_env_iterator nh)
{
    while (*nh != '\0')
        nh ++;
    return ++nh ;
}
native_env_iterator find_end(native_env_iterator nh)
{
    while (*nh != EOF)
        nh ++;
    return nh ;
}

const environment::char_type * dereference(native_env_iterator iterator)
{
    return iterator;
}

#elif (defined(__APPLE___) || defined(__MACH__)) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__sun)

void native_env_handle_deleter::operator()(native_env_handle_type h) const
{
    delete [] h;
}

native_env_iterator next(native_env_iterator nh)
{
    while (*nh != '\0')
        nh ++;
    return ++nh ;
}
native_env_iterator find_end(native_env_iterator nh)
{
    while (*nh - 1 != '\0' && *nh != '\0')
        nh ++;
    return nh ;
}

const environment::char_type * dereference(native_env_iterator iterator)
{
    return iterator;
}

#endif

}
}

namespace ext
{

#if defined(BOOST_PROCESS_V2_WINDOWS)

env_view env(HANDLE proc, error_code & ec)
{
    wchar_t *buffer = nullptr;
    PEB peb;
    SIZE_T nRead = 0; 
    ULONG len = 0;
    PROCESS_BASIC_INFORMATION pbi;
    detail::ext::RTL_USER_PROCESS_PARAMETERS_EXTENDED upp;

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

    env_view ev;
    buf = upp.Environment;
    len = (ULONG)upp.EnvironmentSize;
    ev.handle_.reset(new wchar_t[len / 2 + 1]());

    if (!ReadProcessMemory(proc, buf, ev.handle_.get(), len, &nRead))
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return {};
    }

    ev.handle_.get()[len / 2] = L'\0';
    return ev;
}

env_view env(HANDLE handle)
{
    error_code ec;
    auto res = env(handle, ec);
    if (ec)
        detail::throw_error(ec, "env");
    return res;
}

env_view env(boost::process::v2::pid_type pid, error_code & ec)
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
	    return env(proc.get(), ec);

	return {};
}

#elif (defined(__APPLE___) || defined(__MACH__)) && !TARGET_OS_IOS

env_view env(boost::process::v2::pid_type pid, error_code & ec)
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
    mib[1] = KERN_PROCARGS2;
    mib[2] = pid;

    size = argmax;

    if (sysctl(mib, 3, &*procargs.begin(), &size, nullptr, 0) != 0)
    {
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        return {};
    }
    std::uint32_t nargs;
    memcpy(&nargs, &*procargs.begin(), sizeof(nargs));
    char *cp = &*procargs.begin() + sizeof(nargs);

    for (; cp < &*procargs.end(); cp++)
        if (*cp == '\0')
          break;


    if (cp == &procargs[size])
        return {};


    for (; cp < &*procargs.end(); cp++)
        if (*cp != '\0') break;


    if (cp == &*procargs.end())
        return {};


    int i = 0;
    char *sp = cp;
    std::vector<char> vec;

    while ((*sp != '\0' || i < nargs) && sp < &*procargs.end()) {
        if (i >= nargs)
            vec.push_back(*sp);

        sp += 1;
    }
    
    env_view ev;
    ev.handle_.reset(new char[vec.size()]());
    std::copy(vec.begin(), vec.end(), ev.handle_.get());
    return ev;
}

#elif (defined(__linux__) || defined(__ANDROID__))

env_view env(boost::process::v2::pid_type pid, error_code & ec)
{
    std::size_t size = 0;
    std::unique_ptr<char, detail::ext::native_env_handle_deleter> procargs{};

    int f = ::open(("/proc/" + std::to_string(pid) + "/environ").c_str(), O_RDONLY);

    while (!procargs || procargs.get()[size - 1] != EOF)
    {
        std::unique_ptr<char, detail::ext::native_env_handle_deleter> buf{new char[size + 4096]};
        if (size > 0)
            std::memmove(buf.get(), procargs.get(), size);
        auto r = ::read(f, buf.get() + size, 4096);
        if (r < 0)
        {
            BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
            ::close(f);
            return {};
        }
        procargs = std::move(buf);
        size += r;
        if (r < 4096) // done!
        {
            procargs.get()[size] = EOF;
            break;
        }
    }
    ::close(f);

    env_view ev;
    ev.handle_ = std::move(procargs);
    return ev;
}

#elif defined(__FreeBSD__) || defined(__DragonFly__)
env_view env(boost::process::v2::pid_type pid, error_code & ec)
{
  std::vector<char> vec;
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
  if ((proc_info = kvm_getprocs(kd.get(), KERN_PROC_PID, pid, &cntp))) {
    char **env = kvm_getenvv(kd.get(), proc_info, 0);
    if (env) {
      for (int i = 0; env[i] != nullptr; i++) 
      {
        for (int j = 0; j < strlen(env[i]); j++)
          vec.push_back(env[i][j]);
        vec.push_back('\0');
      }
      vec.push_back('\0');
    }
    else
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
  }
  else
    BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);

  env_view ev;
  ev.handle_.reset(new char[vec.size()]());
  std::copy(vec.begin(), vec.end(), ev.handle_.get());
  return ev;
}

#elif defined(__NetBSD__)
env_view env(boost::process::v2::pid_type pid, error_code & ec)
{
  std::vector<char> vec;
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
  if ((proc_info = kvm_getproc2(kd.get(), KERN_PROC_PID, pid, sizeof(struct kinfo_proc2), &cntp))) {
    char **env = kvm_getenvv2(kd.get(), proc_info, 0);
    if (env) {
      for (int i = 0; env[i] != nullptr; i++) 
      {
        for (int j = 0; j < strlen(env[i]); j++)
          vec.push_back(env[i][j]);
        vec.push_back('\0');
      }
      vec.push_back('\0');
    }
    else
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
  }
  else
    BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);

  env_view ev;
  ev.handle_.reset(new char[vec.size()]());
  std::copy(vec.begin(), vec.end(), ev.handle_.get());
  return ev;
}
#elif defined(__OpenBSD__)
env_view env(boost::process::v2::pid_type pid, error_code & ec)
{
  std::vector<char> vec;
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
    char **env = kvm_getenvv(kd.get(), proc_info, 0);
    if (env)
    {
      for (int i = 0; env[i] != nullptr; i++) 
      {
        for (int j = 0; j < strlen(env[i]); j++)
          vec.push_back(env[i][j]);
        vec.push_back('\0');
      }
      vec.push_back('\0');
    }
    else
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
  }
  else
    BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);

  env_view ev;
  ev.handle_.reset(new char[vec.size()]());
  std::copy(vec.begin(), vec.end(), ev.handle_.get());
  return ev;
}
#elif defined(__sun)
env_view env(boost::process::v2::pid_type pid, error_code & ec)
{
  std::vector<char> vec;
  char **env = nullptr;
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
      if (!kvm_getcmd(kd.get(), proc_info, proc_user, nullptr, &env)) 
      {
        for (int i = 0; env[i] != nullptr; i++) 
        {
          for (int j = 0; j < strlen(env[i]); j++)
            vec.push_back(env[i][j]);
          vec.push_back('\0');
        }
        vec.push_back('\0');
        free(env);
      }
      else
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
    }
    else
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
  }
  else
    BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);

  env_view ev;
  ev.handle_.reset(new char[vec.size()]());
  std::copy(vec.begin(), vec.end(), ev.handle_.get());
  return ev;
}
#endif

env_view env(boost::process::v2::pid_type pid)
{
    error_code ec;
    auto res = env(pid, ec);
    if (ec)
        detail::throw_error(ec, "env");
    return res;
}
}
BOOST_PROCESS_V2_END_NAMESPACE
