/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <sys/time.h>
#include <string_view>

/*
 * current_time --
 *     Return the current time in seconds.
 */
inline double
current_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return tv.tv_sec + tv.tv_usec / 1.0e6;
}

/*
 * ends_with --
 *     Check whether the string ends with the given suffix.
 */
inline bool
ends_with(std::string_view str, std::string_view suffix)
{
    return str.size() >= suffix.size() &&
      str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}
