//  Copyright 2022 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

#include "platform_config.hpp"

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

int main()
{
    int fd = open(".", O_DIRECTORY | O_RDONLY | O_NOFOLLOW);
    DIR* dir = fdopendir(fd);
    return dir != 0;
}
