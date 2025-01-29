/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "utils_sub_level_error.h"

namespace utils {

/*
 * check_error_info --
 *     Perform checks on each of the three members of a WT_ERROR_INFO struct.
 */
void
check_error_info(WT_ERROR_INFO *err_info, int err, int sub_level_err, const char *err_msg_content)
{
    CHECK(err_info->err == err);
    CHECK(err_info->sub_level_err == sub_level_err);
    CHECK(strcmp(err_info->err_msg, err_msg_content) == 0);
}

} // namespace utils
