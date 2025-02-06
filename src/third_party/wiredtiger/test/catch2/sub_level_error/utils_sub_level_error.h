/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <catch2/catch.hpp>
#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

namespace utils {

void prepare_session_and_error(
  connection_wrapper *conn_wrapper, WT_SESSION **session, WT_ERROR_INFO **err_info);

void check_error_info(
  WT_ERROR_INFO *err_info, int err, int sub_level_err, const char *err_msg_content);

} // namespace utils
