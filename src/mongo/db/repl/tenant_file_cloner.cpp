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


#include "mongo/platform/basic.h"

#include <fstream>

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_file_cloner.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/logv2/log.h"

#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo::repl {

// TODO SERVER-63119: Verify if we need the below failpoints for TenantFileCloner unit testing.
// Failpoint which causes the file cloner to hang after handling the next batch of results
// from the DBClientConnection, optionally limited to a specific file name.
MONGO_FAIL_POINT_DEFINE(TenantFileClonerHangAfterHandlingBatchResponse);
MONGO_FAIL_POINT_DEFINE(TenantFileClonerHangDuringFileCloneBackup);
MONGO_FAIL_POINT_DEFINE(TenantFileClonerDisableExhaust);
TenantFileCloner::TenantFileCloner(const UUID& backupId,
                                   const UUID& migrationId,
                                   const std::string& remoteFileName,
                                   size_t remoteFileSize,
                                   const std::string& relativePath,
                                   TenantMigrationSharedData* sharedData,
                                   const HostAndPort& source,
                                   DBClientConnection* client,
                                   StorageInterface* storageInterface,
                                   ThreadPool* dbPool)
    : TenantBaseCloner("TenantFileCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _backupId(backupId),
      _migrationId(migrationId),
      _remoteFileName(remoteFileName),
      _remoteFileSize(remoteFileSize),
      _relativePathString(relativePath),
      _queryStage("query", this, &TenantFileCloner::queryStage),
      _fsWorkTaskRunner(dbPool),
      _scheduleFsWorkFn([this](executor::TaskExecutor::CallbackFn work) {
          auto task = [ this, work = std::move(work) ](
                          OperationContext * opCtx,
                          const Status& status) mutable noexcept->TaskRunner::NextAction {
              try {
                  work(executor::TaskExecutor::CallbackArgs(nullptr, {}, status, opCtx));
              } catch (const DBException& e) {
                  setSyncFailedStatus(e.toStatus());
              }
              return TaskRunner::NextAction::kDisposeOperationContext;
          };
          _fsWorkTaskRunner.schedule(std::move(task));
          return executor::TaskExecutor::CallbackHandle();
      }),
      _progressMeter(remoteFileSize,
                     kProgressMeterSecondsBetween,
                     kProgressMeterCheckInterval,
                     "bytes copied",
                     str::stream() << _remoteFileName << " Tenant migration file clone progress") {
    _stats.filePath = _relativePathString;
    _stats.fileSize = _remoteFileSize;
}

BaseCloner::ClonerStages TenantFileCloner::getStages() {
    return {&_queryStage};
}

void TenantFileCloner::preStage() {
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.start = getSharedData()->getClock()->now();

    // Construct local path name from the relative path and the temp dbpath.
    boost::filesystem::path relativePath(_relativePathString);
    uassert(6113300,
            str::stream() << "Path " << _relativePathString << " should be a relative path",
            relativePath.is_relative());

    auto syncTargetTempDBPath = shard_merge_utils::fileClonerTempDir(_migrationId);
    _localFilePath = syncTargetTempDBPath;

    _localFilePath /= relativePath;
    _localFilePath = _localFilePath.lexically_normal();
    uassert(6113301,
            str::stream() << "Path " << _relativePathString
                          << " must not escape its parent directory.",
            StringData(_localFilePath.generic_string())
                .startsWith(syncTargetTempDBPath.generic_string()));

    // Create and open files and any parent directories.
    if (boost::filesystem::exists(_localFilePath)) {
        LOGV2(6113302,
              "Local file exists at start of TenantFileCloner; truncating.",
              "localFilePath"_attr = _localFilePath.string());
    } else {
        auto localFileDir = _localFilePath.parent_path();
        boost::system::error_code ec;
        boost::filesystem::create_directories(localFileDir, ec);
        uassert(6113303,
                str::stream() << "Failed to create directory " << localFileDir.string() << " Error "
                              << ec.message(),
                !ec);
    }
    _localFile.open(_localFilePath.string(),
                    std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
    uassert(ErrorCodes::FileOpenFailed,
            str::stream() << "Failed to open file " << _localFilePath.string(),
            !_localFile.fail());
    _fileOffset = 0;
}

void TenantFileCloner::postStage() {
    _localFile.close();
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.end = getSharedData()->getClock()->now();
}

BaseCloner::AfterStageBehavior TenantFileCloner::queryStage() {
    // Since the query stage may be re-started, we need to make sure all the file system work
    // from the previous run is done before running the query again.
    waitForFilesystemWorkToComplete();
    _sawEof = false;
    runQuery();
    waitForFilesystemWorkToComplete();
    uassert(
        6113304,
        str::stream()
            << "Received entire file, but did not get end of file marker. File may be incomplete "
            << _localFilePath.string(),
        _sawEof);
    return kContinueNormally;
}

size_t TenantFileCloner::getFileOffset() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _fileOffset;
}

void TenantFileCloner::runQuery() {
    auto backupFileStage = BSON(
        "$_backupFile" << BSON("backupId" << _backupId << "file" << _remoteFileName << "byteOffset"
                                          << static_cast<int64_t>(getFileOffset())));
    AggregateCommandRequest aggRequest(
        NamespaceString::makeCollectionlessAggregateNSS(NamespaceString::kAdminDb),
        {backupFileStage});
    aggRequest.setReadConcern(ReadConcernArgs::kImplicitDefault);
    aggRequest.setWriteConcern(WriteConcernOptions());

    LOGV2_DEBUG(6113305,
                2,
                "TenantFileCloner running aggregation",
                "source"_attr = getSource(),
                "aggRequest"_attr = aggregation_request_helper::serializeToCommandObj(aggRequest));
    const bool useExhaust = !MONGO_unlikely(TenantFileClonerDisableExhaust.shouldFail());
    std::unique_ptr<DBClientCursor> cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        getClient(), std::move(aggRequest), true /* secondaryOk */, useExhaust));
    try {
        while (cursor->more()) {
            handleNextBatch(*cursor);
        }
    } catch (const DBException& e) {
        // We cannot continue after an error when processing exhaust cursors. Instead we must
        // reconnect, which is handled by the BaseCloner.
        LOGV2_DEBUG(6113306,
                    1,
                    "TenantFileCloner received an exception while downloading data",
                    "error"_attr = e.toStatus(),
                    "source"_attr = getSource(),
                    "backupId"_attr = _backupId,
                    "remoteFile"_attr = _remoteFileName,
                    "fileOffset"_attr = getFileOffset());
        getClient()->shutdown();
        throw;
    }
}

void TenantFileCloner::handleNextBatch(DBClientCursor& cursor) {
    LOGV2_DEBUG(6113307,
                3,
                "TenantFileCloner handleNextBatch",
                "source"_attr = getSource(),
                "backupId"_attr = _backupId,
                "remoteFile"_attr = _remoteFileName,
                "fileOffset"_attr = getFileOffset(),
                "moreInCurrentBatch"_attr = cursor.moreInCurrentBatch());
    {
        stdx::lock_guard<TenantMigrationSharedData> lk(*getSharedData());
        if (!getSharedData()->getStatus(lk).isOK()) {
            static constexpr char message[] = "BackupFile cloning cancelled due to cloning failure";
            LOGV2(6113323, message, "error"_attr = getSharedData()->getStatus(lk));
            uasserted(ErrorCodes::CallbackCanceled,
                      str::stream() << message << ": " << getSharedData()->getStatus(lk));
        }
    }
    while (cursor.moreInCurrentBatch()) {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.receivedBatches++;
        while (cursor.moreInCurrentBatch()) {
            _dataToWrite.emplace_back(cursor.nextSafe());
        }
    }

    // Schedule the next set of writes.
    auto&& scheduleResult = _scheduleFsWorkFn([=](const executor::TaskExecutor::CallbackArgs& cbd) {
        writeDataToFilesystemCallback(cbd);
    });

    if (!scheduleResult.isOK()) {
        Status newStatus = scheduleResult.getStatus().withContext(
            str::stream() << "Error copying file '" << _remoteFileName << "'");
        // We must throw an exception to terminate query.
        uassertStatusOK(newStatus);
    }

    TenantFileClonerHangAfterHandlingBatchResponse.executeIf(
        [&](const BSONObj&) {
            while (MONGO_unlikely(TenantFileClonerHangAfterHandlingBatchResponse.shouldFail()) &&
                   !mustExit()) {
                LOGV2(6113308,
                      "TenantFileClonerHangAfterHandlingBatchResponse fail point "
                      "enabled. Blocking until fail point is disabled",
                      "remoteFile"_attr = _remoteFileName);
                mongo::sleepmillis(100);
            }
        },
        [&](const BSONObj& data) {
            // Only hang when copying the specified file, or if no file was specified.
            auto filename = data["remoteFile"].str();
            return filename.empty() || filename == _remoteFileName;
        });
}

void TenantFileCloner::writeDataToFilesystemCallback(
    const executor::TaskExecutor::CallbackArgs& cbd) {
    LOGV2_DEBUG(6113309,
                3,
                "TenantFileCloner writeDataToFilesystemCallback",
                "backupId"_attr = _backupId,
                "remoteFile"_attr = _remoteFileName,
                "localFile"_attr = _localFilePath.string(),
                "fileOffset"_attr = getFileOffset());
    uassertStatusOK(cbd.status);
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (_dataToWrite.size() == 0) {
            LOGV2_WARNING(6113310,
                          "writeDataToFilesystemCallback, but no data to write",
                          "remoteFile"_attr = _remoteFileName);
        }
        for (const auto& doc : _dataToWrite) {
            uassert(6113311,
                    str::stream() << "Saw multiple end-of-file-markers in file " << _remoteFileName,
                    !_sawEof);
            // Received file data should always be in sync with the stream and where we think
            // our next input should be coming from.
            const auto byteOffset = doc["byteOffset"].safeNumberLong();
            invariant(byteOffset == _localFile.tellp());
            invariant(byteOffset == _fileOffset);
            const auto& dataElem = doc["data"];
            uassert(6113312,
                    str::stream() << "Expected file data to be type BinDataGeneral. " << doc,
                    dataElem.type() == BinData && dataElem.binDataType() == BinDataGeneral);
            int dataLength;
            auto data = dataElem.binData(dataLength);
            _localFile.write(data, dataLength);
            uassert(ErrorCodes::FileStreamFailed,
                    str::stream() << "Unable to write file data for file " << _remoteFileName
                                  << " at offset " << _fileOffset,
                    !_localFile.fail());
            _progressMeter.hit(dataLength);
            _fileOffset += dataLength;
            _stats.bytesCopied += dataLength;
            _sawEof = doc["endOfFile"].booleanSafe();
        }
        _dataToWrite.clear();
        _stats.writtenBatches++;
    }

    TenantFileClonerHangDuringFileCloneBackup.executeIf(
        [&](const BSONObj&) {
            LOGV2(6113313,
                  "TenantFileClonerHangDuringFileCloneBackup fail point "
                  "enabled. Blocking until fail point is disabled");
            while (MONGO_unlikely(TenantFileClonerHangDuringFileCloneBackup.shouldFail()) &&
                   !mustExit()) {
                mongo::sleepmillis(100);
            }
        },
        [&](const BSONObj& data) {
            return (data["remoteFile"].eoo() || data["remoteFile"].str() == _remoteFileName) &&
                (data["fileOffset"].eoo() || data["fileOffset"].safeNumberLong() <= _fileOffset);
        });
}

bool TenantFileCloner::isMyFailPoint(const BSONObj& data) const {
    auto remoteFile = data["remoteFile"].str();
    return (remoteFile.empty() || remoteFile == _remoteFileName) && BaseCloner::isMyFailPoint(data);
}

void TenantFileCloner::waitForFilesystemWorkToComplete() {
    _fsWorkTaskRunner.join();
}

TenantFileCloner::Stats TenantFileCloner::getStats() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _stats;
}

std::string TenantFileCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj TenantFileCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    append(&bob);
    return bob.obj();
}

void TenantFileCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->append("filePath", filePath);
    builder->appendNumber("fileSize", static_cast<long long>(fileSize));
    builder->appendNumber("bytesCopied", static_cast<long long>(bytesCopied));
    if (start != Date_t()) {
        builder->appendDate("start", start);
        if (end != Date_t()) {
            builder->appendDate("end", end);
            auto elapsed = end - start;
            long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
            builder->appendNumber("elapsedMillis", elapsedMillis);
        }
    }
    builder->appendNumber("receivedBatches", static_cast<long long>(receivedBatches));
    builder->appendNumber("writtenBatches", static_cast<long long>(writtenBatches));
}

}  // namespace mongo::repl
