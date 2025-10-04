/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <string>

#include "live_restore_test_env.h"

namespace utils {

typedef enum { DEST, NO_DEST } HasDest;
typedef enum { SOURCE, NO_SOURCE } HasSource;
typedef enum { MIGRATING, NOT_MIGRATING } IsMigrating;
typedef enum { STOP, NO_STOP } HasStop;

// File op helpers
void create_file(const std::string &filepath, int len = 1, char fill_char = 'A');
int open_lr_fh(const live_restore_test_env &env, const std::string &dest_file,
  WTI_LIVE_RESTORE_FILE_HANDLE **lr_fhp, int flags = 0);

} // namespace utils.
