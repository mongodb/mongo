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

#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(backupCursorErrorAfterOpen);

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

    // Replica sets must also return the opTime's of the earliest and latest oplog entry. The
    // range represented by the oplog start/end values must exist in the backup copy, but are not
    // expected to be exact.
    repl::OpTime oplogStart;
    repl::OpTime oplogEnd;

    // If the oplog exists, capture the last oplog entry before opening the backup cursor. This
    // value will be checked again after the cursor is established to guarantee it still exists
    // (and was not truncated before the backup cursor was established.
    {
        AutoGetCollectionForRead coll(opCtx, NamespaceString::kRsOplogNamespace);
        if (coll.getCollection()) {
            BSONObj lastEntry;
            uassert(50914,
                    str::stream() << "No oplog records were found.",
                    Helpers::getLast(
                        opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), lastEntry));
            auto oplogEntry = fassertNoTrace(50913, repl::OplogEntry::parse(lastEntry));
            oplogEnd = oplogEntry.getOpTime();
        }
    }

    // Capture the checkpointTimestamp before and after opening a cursor. If it hasn't moved, the
    // checkpointTimestamp is known to be exact. If it has moved, uassert and have the user retry.
    boost::optional<Timestamp> checkpointTimestamp;
    if (_storageEngine->supportsRecoverToStableTimestamp()) {
        checkpointTimestamp = _storageEngine->getLastStableRecoveryTimestamp();
    };

    auto filesToBackup = uassertStatusOK(_storageEngine->beginNonBlockingBackup(opCtx));
    _state = kBackupCursorOpened;
    _openCursor = ++_cursorIdGenerator;
    log() << "Opened backup cursor. ID: " << _openCursor.get();

    // A backup cursor is open. Any exception code path must leave the BackupCursorService in an
    // inactive state.
    auto closeCursorGuard =
        MakeGuard([this, opCtx, &lk] { _closeBackupCursor(opCtx, _openCursor.get(), lk); });

    uassert(50919,
            "Failpoint hit after opening the backup cursor.",
            !MONGO_FAIL_POINT(backupCursorErrorAfterOpen));

    // Ensure the checkpointTimestamp hasn't moved. A subtle case to catch is the first stable
    // checkpoint coming out of initial sync racing with opening the backup cursor.
    if (checkpointTimestamp && _storageEngine->supportsRecoverToStableTimestamp()) {
        auto requeriedCheckpointTimestamp = _storageEngine->getLastStableRecoveryTimestamp();
        if (!requeriedCheckpointTimestamp ||
            requeriedCheckpointTimestamp.get() < checkpointTimestamp.get()) {
            severe() << "The last stable recovery timestamp went backwards. Original: "
                     << checkpointTimestamp.get() << " Found: " << requeriedCheckpointTimestamp;
            fassertFailed(50916);
        }

        uassert(50915,
                str::stream() << "A checkpoint took place while opening a backup cursor.",
                checkpointTimestamp == requeriedCheckpointTimestamp);
    };

    // If the oplog exists, capture the first oplog entry after opening the backup cursor. Ensure
    // it is before the `oplogEnd` value.
    if (!oplogEnd.isNull()) {
        BSONObj firstEntry;
        uassert(50912,
                str::stream() << "No oplog records were found.",
                Helpers::getSingleton(
                    opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), firstEntry));
        auto oplogEntry = fassertNoTrace(50918, repl::OplogEntry::parse(firstEntry));
        oplogStart = oplogEntry.getOpTime();
        uassert(50917,
                str::stream() << "Oplog rolled over while establishing the backup cursor.",
                oplogStart < oplogEnd);
    }

    auto encHooks = EncryptionHooks::get(opCtx->getServiceContext());
    if (encHooks->enabled()) {
        auto eseFiles = uassertStatusOK(encHooks->beginNonBlockingBackup());
        filesToBackup.insert(filesToBackup.end(), eseFiles.begin(), eseFiles.end());
    }

    BSONObjBuilder builder;
    if (!oplogStart.isNull()) {
        builder << "oplogStart" << oplogStart.toBSON();
        builder << "oplogEnd" << oplogEnd.toBSON();
    }

    // Notably during initial sync, a node may have an oplog without a stable checkpoint.
    if (checkpointTimestamp) {
        builder << "checkpointTimestamp" << checkpointTimestamp.get();
    }

    boost::optional<Document> preamble = boost::none;
    if (!oplogStart.isNull()) {
        preamble = Document{{"metadata", builder.obj()}};
    }

    closeCursorGuard.Dismiss();
    return {_openCursor.get(), preamble, filesToBackup};
}

void BackupCursorService::closeBackupCursor(OperationContext* opCtx, std::uint64_t cursorId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _closeBackupCursor(opCtx, cursorId, lk);
}

void BackupCursorService::_closeBackupCursor(OperationContext* opCtx,
                                             std::uint64_t cursorId,
                                             WithLock) {
    uassert(50880, "There is no backup cursor to close.", _state == kBackupCursorOpened);
    uassert(50879,
            str::stream() << "Can only close the running backup cursor. To close: " << cursorId
                          << " Running: "
                          << _openCursor.get(),
            cursorId == _openCursor.get());
    _storageEngine->endNonBlockingBackup(opCtx);
    auto encHooks = EncryptionHooks::get(opCtx->getServiceContext());
    if (encHooks->enabled()) {
        fassert(50934, encHooks->endNonBlockingBackup());
    }
    log() << "Closed backup cursor. ID: " << cursorId;
    _state = kInactive;
    _openCursor = boost::none;
}

}  // namespace mongo
