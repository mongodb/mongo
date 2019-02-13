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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/backup_cursor_hooks.h"

#include "mongo/base/init.h"
#include "mongo/db/service_context.h"

namespace mongo {

namespace {
BackupCursorHooks::InitializerFunction initializer = [](StorageEngine* storageEngine) {
    return stdx::make_unique<BackupCursorHooks>();
};

struct BackupCursorHooksHolder {
    std::unique_ptr<BackupCursorHooks> ptr = std::make_unique<BackupCursorHooks>();
};

const auto getBackupCursorHooks = ServiceContext::declareDecoration<BackupCursorHooksHolder>();
}  // namespace

void BackupCursorHooks::registerInitializer(InitializerFunction func) {
    initializer = func;
}

void BackupCursorHooks::initialize(ServiceContext* service, StorageEngine* storageEngine) {
    getBackupCursorHooks(service).ptr = initializer(storageEngine);
}

BackupCursorHooks* BackupCursorHooks::get(ServiceContext* service) {
    return getBackupCursorHooks(service).ptr.get();
}

BackupCursorHooks::~BackupCursorHooks() {}

bool BackupCursorHooks::enabled() const {
    return false;
}

/**
 * The following methods cannot be called when BackupCursorHooks is not enabled.
 */
void BackupCursorHooks::fsyncLock(OperationContext* opCtx) {
    MONGO_UNREACHABLE;
}

void BackupCursorHooks::fsyncUnlock(OperationContext* opCtx) {
    MONGO_UNREACHABLE;
}

BackupCursorState BackupCursorHooks::openBackupCursor(OperationContext* opCtx) {
    MONGO_UNREACHABLE;
}

void BackupCursorHooks::closeBackupCursor(OperationContext* opCtx, const UUID& backupId) {
    MONGO_UNREACHABLE;
}

BackupCursorExtendState BackupCursorHooks::extendBackupCursor(OperationContext* opCtx,
                                                              const UUID& backupId,
                                                              const Timestamp& extendTo) {
    MONGO_UNREACHABLE;
}

bool BackupCursorHooks::isBackupCursorOpen() const {
    return false;
}
}  // namespace mongo
