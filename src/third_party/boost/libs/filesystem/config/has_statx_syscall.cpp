//  Copyright 2020 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

#include "platform_config.hpp"

#include <sys/syscall.h>
#include <linux/stat.h>

// Note: Include other libc headers for stat() as well to ensure there is no conflict between
//       Linux kernel headers and libc headers.
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#if !defined(__NR_statx)
#error "No statx syscall"
#endif

int main()
{
    struct statx st;
    int res = syscall(__NR_statx, AT_FDCWD, ".", AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_BTIME, &st);
    st.stx_btime.tv_sec = 1;
    st.stx_btime.tv_nsec = 10;
}
