/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <string>
#include "wt_internal.h"

#define DB_HOME "test_db"

#define BREAK utils::break_here(__FILE__, __func__, __LINE__)

namespace utils {
void break_here(const char *file, const char *func, int line);
void throw_if_non_zero(int result);
void wiredtiger_cleanup(const std::string &db_home);
void check_error_info(
  WT_ERROR_INFO *err_info, int err, int sub_level_err, const char *err_msg_content);
} // namespace utils.
