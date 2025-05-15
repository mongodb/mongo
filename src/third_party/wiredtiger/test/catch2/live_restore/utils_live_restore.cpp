/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "utils_live_restore.h"
#include <fstream>
namespace utils {

// Open the live restore file handle for a file. This file path is identical to the backing file in
// the destination folder.
int
open_lr_fh(const live_restore_test_env &env, const std::string &dest_file,
  WTI_LIVE_RESTORE_FILE_HANDLE **lr_fhp, int flags)
{
    WTI_LIVE_RESTORE_FS *lr_fs = env.lr_fs;
    WT_SESSION *wt_session = reinterpret_cast<WT_SESSION *>(env.session);
    // Make sure we're always opening the file in the destination directory.
    REQUIRE(strncmp(dest_file.c_str(), env.DB_DEST.c_str(), env.DB_DEST.size()) == 0);
    return lr_fs->iface.fs_open_file(reinterpret_cast<WT_FILE_SYSTEM *>(lr_fs), wt_session,
      dest_file.c_str(), WT_FS_OPEN_FILE_TYPE_DATA, flags,
      reinterpret_cast<WT_FILE_HANDLE **>(lr_fhp));
}

/* Create a file of the specified length. */
void
create_file(const std::string &filepath, int len, char fill_char)
{
    REQUIRE(!testutil_exists(nullptr, filepath.c_str()));
    std::ofstream file(filepath, std::ios::out);
    std::string data_str = std::string(len, fill_char);
    file << data_str;
    file.close();
}

} // namespace utils.
