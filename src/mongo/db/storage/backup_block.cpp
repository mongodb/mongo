/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/backup_block.h"

#include "mongo/base/string_data.h"
#include "mongo/db/storage/storage_options.h"

#include <set>

#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

const std::set<std::string> kRequiredWTFiles = {
    "WiredTiger", "WiredTiger.backup", "WiredTigerHS.wt"};

const std::set<std::string> kRequiredMDBFiles = {"_mdb_catalog.wt", "sizeStorer.wt"};

}  // namespace

BackupBlock::BackupBlock(boost::optional<NamespaceString> nss,
                         boost::optional<UUID> uuid,
                         KVBackupBlock kvBackupBlock)
    : _nss(nss), _uuid(uuid), _kvBackupBlock(kvBackupBlock) {}

bool BackupBlock::isRequired() const {
    const std::string filename = _kvBackupBlock.fileName();

    // Check whether this is a required WiredTiger file.
    if (kRequiredWTFiles.find(filename) != kRequiredWTFiles.end()) {
        return true;
    }

    // Check if this is a journal file.
    if (StringData(filename).starts_with("WiredTigerLog.")) {
        return true;
    }

    // Check whether this is a required MongoDB file.
    if (kRequiredMDBFiles.find(filename) != kRequiredMDBFiles.end()) {
        return true;
    }

    // All files for the encrypted storage engine are required.
    boost::filesystem::path basePath(storageGlobalParams.dbpath);
    boost::filesystem::path keystoreBasePath(basePath / "key.store");
    if (StringData(_kvBackupBlock.filePath()).starts_with(keystoreBasePath.string())) {
        return true;
    }

    if (!_nss) {
        return false;
    }

    // Check if collection resides in an internal database (admin, local, or config).
    if (_nss->isOnInternalDb()) {
        return true;
    }

    // Check if collection is 'system.views'.
    if (_nss->isSystemDotViews()) {
        return true;
    }

    return false;
}

}  // namespace mongo
