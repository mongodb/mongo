/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/backup_cursor_service.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto getBackupCursorService =
    ServiceContext::declareDecoration<std::unique_ptr<BackupCursorService>>();
}  // namespace

BackupCursorService* BackupCursorService::get(ServiceContext* service) {
    return getBackupCursorService(service).get();
}

void BackupCursorService::set(ServiceContext* service,
                              std::unique_ptr<BackupCursorService> backupCursorService) {
    auto& decoration = getBackupCursorService(service);
    decoration = std::move(backupCursorService);
}

void BackupCursorService::fsyncLock(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    uassert(50885, "The node is already fsyncLocked.", _state != kFsyncLocked);
    uassert(50884,
            "The existing backup cursor must be closed before fsyncLock can succeed.",
            _state != kBackupCursorOpened);
    uassertStatusOK(_storageEngine->beginBackup(opCtx));
    _state = kFsyncLocked;
}

void BackupCursorService::fsyncUnlock(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    uassert(50888, "The node is not fsyncLocked.", _state == kFsyncLocked);
    _storageEngine->endBackup(opCtx);
    _state = kInactive;
}

BackupCursorState BackupCursorService::openBackupCursor(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    uassert(50887, "The node is currently fsyncLocked.", _state != kFsyncLocked);
    uassert(50886,
            "The existing backup cursor must be closed before $backupCursor can succeed.",
            _state != kBackupCursorOpened);
    auto filesToBackup = uassertStatusOK(_storageEngine->beginNonBlockingBackup(opCtx));
    _state = kBackupCursorOpened;
    _openCursor = ++_cursorIdGenerator;
    log() << "Opened backup cursor. ID: " << _openCursor.get();

    return {_openCursor.get(), filesToBackup};
}

void BackupCursorService::closeBackupCursor(OperationContext* opCtx, std::uint64_t cursorId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    uassert(50880, "There is no backup cursor to close.", _state == kBackupCursorOpened);
    uassert(50879,
            str::stream() << "Can only close the running backup cursor. To close: " << cursorId
                          << " Running: "
                          << _openCursor.get(),
            cursorId == _openCursor.get());
    _storageEngine->endNonBlockingBackup(opCtx);
    log() << "Closed backup cursor. ID: " << cursorId;
    _state = kInactive;
    _openCursor = boost::none;
}

}  // namespace mongo
