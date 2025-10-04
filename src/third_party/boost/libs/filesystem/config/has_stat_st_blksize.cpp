//  Copyright 2021 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

#include "platform_config.hpp"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

int main()
{
    struct stat st;
    st.st_blksize = 10;
}
