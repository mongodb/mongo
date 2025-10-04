/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <catch2/catch.hpp>
#include "../utils.h"

extern "C" {
#include "wt_internal.h"
#include "test_util.h"
#include "../../../../src/live_restore/live_restore_private.h"
}

#include "../wrappers/connection_wrapper.h"

namespace utils {

class live_restore_test_env {
public:
    const std::string DB_DEST = "WT_LR_DEST";
    const std::string DB_SOURCE = "WT_LR_SOURCE";

    WTI_LIVE_RESTORE_FS *lr_fs;
    std::unique_ptr<connection_wrapper> conn;
    WT_SESSION_IMPL *session;

    live_restore_test_env();

    std::string dest_file_path(const std::string &file_name);
    std::string source_file_path(const std::string &file_name);
    std::string tombstone_file_path(const std::string &file_name);
};

} // namespace utils.
