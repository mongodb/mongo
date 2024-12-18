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

/*
 * Create a string representation of an extent list, for example (1-10),(15-30) represents an extent
 * list with holes at ranges 1 to 10 and 15 to 30 (inclusive).
 */
std::string
extent_list_str(WT_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_LIVE_RESTORE_HOLE_NODE *ext = lr_fh->destination.hole_list_head;

    std::string str = "";

    if (ext == nullptr)
        // nullptr is an empty list
        return "";

    while (ext != nullptr) {
        str += "(" + std::to_string(ext->off) + "-" + std::to_string(WT_EXTENT_END(ext)) + "), ";
        ext = ext->next;
    }

    // Remove the trailing ", "
    str.erase(str.size() - 2);

    return str;
}

// Open the live restore file handle for a file. This file path is identical to the backing file in
// the destination folder.
int
open_lr_fh(const live_restore_test_env &env, const std::string &dest_file,
  WT_LIVE_RESTORE_FILE_HANDLE **lr_fhp)
{
    WT_LIVE_RESTORE_FS *lr_fs = env.lr_fs;
    WT_SESSION *wt_session = reinterpret_cast<WT_SESSION *>(env.session);
    // Make sure we're always opening the file in the destination directory.
    REQUIRE(strncmp(dest_file.c_str(), env.DB_DEST.c_str(), env.DB_DEST.size()) == 0);
    return lr_fs->iface.fs_open_file(reinterpret_cast<WT_FILE_SYSTEM *>(lr_fs), wt_session,
      dest_file.c_str(), WT_FS_OPEN_FILE_TYPE_REGULAR, 0,
      reinterpret_cast<WT_FILE_HANDLE **>(lr_fhp));
}

/* Verify that all extents in an extent list are in order and don't overlap. */
bool
extent_list_in_order(WT_LIVE_RESTORE_FILE_HANDLE *lr_fh)
{
    WT_LIVE_RESTORE_HOLE_NODE *node;
    node = lr_fh->destination.hole_list_head;

    while (node != NULL && node->next != NULL) {
        if (WT_EXTENT_END(node) >= node->next->off)
            return false;

        node = node->next;
    }
    return true;
}

/* Create a file of the specified length. */
void
create_file(const std::string &filepath, int len)
{
    REQUIRE(!testutil_exists(nullptr, filepath.c_str()));
    std::ofstream file(filepath, std::ios::out);
    std::string data_str = std::string(len, 'A');
    file << data_str;
    file.close();
}

} // namespace utils.
