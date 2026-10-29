// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv_backup_block.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Represents the file blocks returned by the storage engine during both full and incremental
 * backups. In the case of a full backup, each block is an entire file holding a KVBackupBlock
 * containing an offset=0 and length=fileSize. In the case of the first basis for future incremental
 * backups, each block is an entire file with offset=0 and length=0. In the case of a subsequent
 * incremental backup, each block reflects changes made to data files since the basis (named
 * 'thisBackupName') and each block has a maximum size of 'blockSizeMB'.
 *
 * If a file is unchanged in a subsequent incremental backup, a single block is returned with
 * offset=0 and length=0. This allows consumers of the backup API to safely truncate files that
 * are not returned by the backup cursor.
 */
class BackupBlock final {
public:
    explicit BackupBlock(boost::optional<NamespaceString> nss,
                         boost::optional<UUID> uuid,
                         KVBackupBlock kvBackupBlock);

    ~BackupBlock() = default;

    KVBackupBlock kvBackupBlock() const {
        return _kvBackupBlock;
    }

    boost::optional<NamespaceString> ns() const {
        return _nss;
    }

    boost::optional<UUID> uuid() const {
        return _uuid;
    }

    /**
     * Returns whether the file must be copied regardless of choice for selective backups.
     */
    bool isRequired() const;

private:
    boost::optional<NamespaceString> _nss;
    boost::optional<UUID> _uuid;
    KVBackupBlock _kvBackupBlock;
};
}  // namespace mongo
