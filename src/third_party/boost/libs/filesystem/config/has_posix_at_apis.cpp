//  Copyright 2022 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

#include "platform_config.hpp"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

int main()
{
    int fd1 = openat(AT_FDCWD, "foo", O_DIRECTORY | O_RDONLY);
    int fd2 = openat(fd1, "bar", O_DIRECTORY | O_RDONLY);
    int res = unlinkat(fd2, "zoo", 0);
    res |= unlinkat(fd1, "bar", AT_REMOVEDIR);
    res |= renameat(AT_FDCWD, "x", fd1, "y");

    struct stat st;
    res |= fstatat(fd1, "y", &st, AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW);

    res |= linkat(fd1, "y", fd1, "z", 0);
    res |= symlinkat("foo/z", fd1, "sz");

    char buf[128u];
    res |= readlinkat(fd1, "sz", buf, sizeof(buf));

    res |= mkdirat(fd1, "dir", S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);

    res |= fchmodat(fd1, "x", S_IRUSR | S_IWUSR, 0);

    struct timespec times[2] = {};
    res |= utimensat(fd1, "x", times, AT_SYMLINK_NOFOLLOW);

    return res != 0;
}
