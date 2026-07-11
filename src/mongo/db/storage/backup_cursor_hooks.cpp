// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/backup_cursor_hooks.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

namespace mongo {

namespace {
BackupCursorHooks::InitializerFunction initializer = []() {
    return std::make_unique<BackupCursorHooks>();
};

struct BackupCursorHooksHolder {
    std::unique_ptr<BackupCursorHooks> ptr = std::make_unique<BackupCursorHooks>();
};

const auto getBackupCursorHooks = ServiceContext::declareDecoration<BackupCursorHooksHolder>();
}  // namespace

void BackupCursorHooks::registerInitializer(InitializerFunction func) {
    initializer = func;
}

void BackupCursorHooks::initialize(ServiceContext* service) {
    getBackupCursorHooks(service).ptr = initializer();
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

BackupCursorState BackupCursorHooks::openBackupCursor(OperationContext* opCtx,
                                                      const StorageEngine::BackupOptions& options) {
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

bool BackupCursorHooks::isFileReturnedByCursor(const UUID& backupId,
                                               boost::filesystem::path filePath) {
    MONGO_UNREACHABLE;
}

void BackupCursorHooks::addFile(const UUID& backupId, boost::filesystem::path filePath) {
    MONGO_UNREACHABLE;
}

}  // namespace mongo
