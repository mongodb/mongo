/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/backup_cursor_state.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <memory>

namespace mongo {
class OperationContext;
class ServiceContext;
class StorageEngine;

class BackupCursorHooks {
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
