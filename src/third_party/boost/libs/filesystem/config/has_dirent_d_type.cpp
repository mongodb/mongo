//  Copyright 2023 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

#include "platform_config.hpp"

#include <sys/types.h>
#include <dirent.h>

int main()
{
    DIR* dir = opendir(".");
    dirent* ent = readdir(dir);
    switch (ent->d_type)
    {
    case DT_REG:
        break;
    case DT_DIR:
        break;
    case DT_LNK:
        break;
    case DT_SOCK:
        break;
    case DT_FIFO:
        break;
    case DT_BLK:
        break;
    case DT_CHR:
        break;
    case DT_UNKNOWN:
        break;
    default:
        break;
    }
    return 0;
}
