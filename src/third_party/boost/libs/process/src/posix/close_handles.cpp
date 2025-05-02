// Copyright (c) 2022 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <boost/process/v2/detail/config.hpp>

#if defined(BOOST_PROCESS_V2_POSIX)

#include <boost/process/v2/detail/last_error.hpp>
#include <boost/process/v2/posix/detail/close_handles.hpp>

#include <algorithm>
#include <memory>

// linux has close_range since 5.19


#if defined(__FreeBSD__)

// https://www.freebsd.org/cgi/man.cgi?query=close_range&apropos=0&sektion=0&manpath=FreeBSD+13.1-RELEASE+and+Ports&arch=default&format=html
// __FreeBSD__
// 
// gives us
//
// int closefrom(int fd);
// int close_range(u_int lowfd, u_int highfd, int flags);

#include <unistd.h>
#define BOOST_PROCESS_V2_HAS_CLOSE_RANGE_AND_CLOSEFROM 1 

#elif defined(__sun)

/*https://docs.oracle.com/cd/E36784_01/html/E36874/closefrom-3c.html

int fdwalk(int (*func)(void *, int), void *cd);
*/

#include <stdlib.h>
#define BOOST_PROCESS_V2_HAS_PDFORK 1 

#elif defined(__linux__)

#include <linux/version.h>

// kernel has close_range
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0) && !defined(BOOST_PROCESS_V2_POSIX_FORCE_DISABLE_CLOSE_RANGE)

// version is included by stdlib.h #include <gnu/libc-version.h>
#if (__GLIBC__ >= 2 && __GLIBC_MINOR__ >= 34) // glibc is compiled with close_range support
// https://man7.org/linux/man-pages/man2/close_range.2.html

#include <linux/close_range.h>
#define BOOST_PROCESS_V2_HAS_CLOSE_RANGE 1
#else

#include <sys/syscall.h>

#if defined(SYS_close_range)

#define BOOST_PROCESS_V2_HAS_CLOSE_RANGE 1
#if !defined(CLOSE_RANGE_UNSHARE)
#define CLOSE_RANGE_UNSHARE 2
#endif

int close_range(unsigned int first, unsigned int last, int flags)
{
  return ::syscall(SYS_close_range, first, last, flags);
}

#else

#include <dirent.h>

#endif

#endif


#else 

#include <dirent.h>

#endif

#else
#include <dirent.h>

#endif


BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace posix
{

namespace detail
{

#if defined(BOOST_PROCESS_V2_HAS_PDFORK)

void close_all(const std::vector<int> & whitelist, error_code & ec)
{
    fdwalk(+[](void * p, int fd)
           {
                const auto & wl = *static_cast<const std::vector<int>*>(p);
                if (std::find(wl.begin(), wl.end(), fd) == wl.end())
                    return ::close(fd);
                else 
                    return 0;
           }, const_cast<void*>(static_cast<const void*>(&whitelist)) );
    ec = BOOST_PROCESS_V2_NAMESPACE::detail::get_last_error();
}

#elif defined(BOOST_PROCESS_V2_HAS_CLOSE_RANGE_AND_CLOSEFROM)

// freeBSD impl - whitelist must be ordered
void close_all(const std::vector<int> & whitelist, error_code & ec)
{
    //the most common scenario is whitelist = {0,1,2}
    if (!whitelist.empty())
    {
        if (whitelist.front() != 0)
            ::close_range(0, whitelist.front() - 1, 0);

        for (std::size_t idx = 0u; 
             idx < (whitelist.size() - 1u);
             idx++)
        {
            const auto mine = whitelist[idx];
            const auto next = whitelist[idx + 1];
            if ((mine + 1) != next && (mine != next))
            {
                ::close_range(mine + 1, next - 1, 0);
            }
        }

        ::closefrom(whitelist.back() + 1);
    }
    else
        ::closefrom(0);
}

#elif defined(BOOST_PROCESS_V2_HAS_CLOSE_RANGE)


// linux impl - whitelist must be ordered
void close_all(const std::vector<int> & whitelist, error_code & /*ec*/)
{
// https://patchwork.kernel.org/project/linux-fsdevel/cover/20200602204219.186620-1-christian.brauner@ubuntu.com/
    //the most common scenario is whitelist = {0,1,2}
    if (!whitelist.empty())
    {
        if (whitelist.front() != 0)
            ::close_range(0, whitelist.front() - 1, CLOSE_RANGE_UNSHARE);

        for (std::size_t idx = 0u; 
             idx < (whitelist.size() - 1u);
             idx++)
        {
            const auto mine = whitelist[idx];
            const auto next = whitelist[idx + 1];
            if ((mine + 1) != next && (mine != next))
            {
                ::close_range(mine + 1, next - 1, CLOSE_RANGE_UNSHARE);
            }
        }

        ::close_range(whitelist.back() + 1, std::numeric_limits<int>::max(), CLOSE_RANGE_UNSHARE);
    }
    else
        ::close_range(0, std::numeric_limits<int>::max(), CLOSE_RANGE_UNSHARE);
}

#else

// default one
void close_all(const std::vector<int> & whitelist, error_code & ec)
{
    std::unique_ptr<DIR, void(*)(DIR*)> dir{::opendir("/dev/fd"), +[](DIR* p){::closedir(p);}};
    if (dir.get() == nullptr)
    {
        ec = BOOST_PROCESS_V2_NAMESPACE::detail::get_last_error();
        return ;
    }

    auto dir_fd = dirfd(dir.get());
    if (dir_fd == -1)
    {
        ec = BOOST_PROCESS_V2_NAMESPACE::detail::get_last_error();
        return ;
    }

    struct ::dirent * ent_p;

    while ((ent_p = ::readdir(dir.get())) != nullptr)
    {
        if (ent_p->d_name[0] == '.')
            continue;

        const auto conv = std::atoi(ent_p->d_name);
        if (conv == 0 && (ent_p->d_name[0] != '0' && ent_p->d_name[1] != '\0'))
            continue;

        if (conv == dir_fd 
            || (std::find(whitelist.begin(), whitelist.end(), conv) != whitelist.end()))
            continue;

        ::close(conv);
    }
}

#endif

}

}

BOOST_PROCESS_V2_END_NAMESPACE

#endif
