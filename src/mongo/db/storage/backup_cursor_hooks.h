// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/backup_cursor_state.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <memory>

namespace mongo {
class OperationContext;
class ServiceContext;
class StorageEngine;

class [[MONGO_MOD_PUBLIC]] BackupCursorHooks {
public:
    using InitializerFunction = std::function<std::unique_ptr<BackupCursorHooks>()>;

    static void registerInitializer(InitializerFunction func);

    static void initialize(ServiceContext* service);

    static BackupCursorHooks* get(ServiceContext* service);

    virtual ~BackupCursorHooks();

    /**
     * Returns true if the backup cursor hooks are enabled. If this returns false, none of the
     * other methods on this class may be called.
     */
    virtual bool enabled() const;

    virtual void fsyncLock(OperationContext* opCtx);

    virtual void fsyncUnlock(OperationContext* opCtx);

    virtual BackupCursorState openBackupCursor(OperationContext* opCtx,
                                               const StorageEngine::BackupOptions& options);

    virtual void closeBackupCursor(OperationContext* opCtx, const UUID& backupId);

    virtual BackupCursorExtendState extendBackupCursor(OperationContext* opCtx,
                                                       const UUID& backupId,
                                                       const Timestamp& extendTo);

    virtual bool isBackupCursorOpen() const;

    /**
     * Returns true if `filePath` was returned by the backup cursor `backupId`.
     * Used to verify files passed into $backupFile.
     */
    virtual bool isFileReturnedByCursor(const UUID& backupId, boost::filesystem::path filePath);

    virtual void addFile(const UUID& backupId, boost::filesystem::path filePath);
};

}  // namespace mongo
