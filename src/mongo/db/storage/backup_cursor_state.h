// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/storage/backup_block.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"

#include <deque>
#include <string>

#include <boost/optional.hpp>

namespace mongo {

struct [[MONGO_MOD_PUBLIC]] BackupCursorState {
    UUID backupId;
    boost::optional<Document> preamble;
    std::unique_ptr<StorageEngine::StreamingCursor> streamingCursor;
    // 'otherBackupBlocks' includes the kv backup blocks for the encrypted storage engine in the
    // enterprise module.
    std::deque<KVBackupBlock> otherKVBackupBlocks;
    stdx::unordered_map<std::string, std::pair<NamespaceString, UUID>> identsToNsAndUUID;
};

struct [[MONGO_MOD_PUBLIC]] BackupCursorExtendState {
    std::deque<std::string> filePaths;
};

}  // namespace mongo
