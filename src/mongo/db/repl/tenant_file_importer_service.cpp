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


#include "mongo/db/repl/tenant_file_importer_service.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/preprocessor/control/iif.hpp>
#include <fmt/format.h>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

MONGO_FAIL_POINT_DEFINE(hangBeforeFileImporterThreadExit);
MONGO_FAIL_POINT_DEFINE(skipCloneFiles);
MONGO_FAIL_POINT_DEFINE(hangBeforeVoteImportedFiles);

namespace mongo::repl {

using namespace fmt::literals;
using namespace shard_merge_utils;

namespace {
const auto _TenantFileImporterService =
    ServiceContext::declareDecoration<TenantFileImporterService>();

const ReplicaSetAwareServiceRegistry::Registerer<TenantFileImporterService>
    _TenantFileImporterServiceRegisterer("TenantFileImporterService");

/**
 * Connect to the donor source.
 */
void connect(const HostAndPort& source, DBClientConnection* client) {
    uassertStatusOK(client->connect(source, "TenantFileImporterService", boost::none));
    uassertStatusOK(replAuthenticate(client).withContext(
        str::stream() << "Failed to authenticate to " << source));
}

}  // namespace

TenantFileImporterService* TenantFileImporterService::get(ServiceContext* serviceContext) {
    return &_TenantFileImporterService(serviceContext);
}

TenantFileImporterService* TenantFileImporterService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

TenantFileImporterService::TenantFileImporterService()
    : _createConnectionFn(
          []() { return std::make_unique<DBClientConnection>(true /* autoReconnect */); }) {}

TenantFileImporterService::MigrationHandle::MigrationHandle(const UUID& migrationId)
    : migrationId(migrationId),
      eventQueue(std::make_unique<Queue>()),
      writerPool(
          makeReplWriterPool(tenantApplierThreadCount, "TenantFileImporterServiceWriter"_sd)),
      sharedData(std::make_unique<TenantMigrationSharedData>(
          getGlobalServiceContext()->getFastClockSource(), migrationId)) {
    stats.fileCopyStart = Date_t::now();
}

void TenantFileImporterService::_makeMigrationHandleIfNotPresent(WithLock,
                                                                 const UUID& migrationId) {
    if (_mh)
        return;
    _mh = std::make_unique<MigrationHandle>(migrationId);
}

void TenantFileImporterService::startMigration(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    if (_isShuttingDown) {
        LOGV2_DEBUG(6690701,
                    3,
                    "TenantFileImporterService:: Not starting migration due to shutdown",
                    "migrationId"_attr = migrationId);
        return;
    }

    _makeMigrationHandleIfNotPresent(lk, migrationId);
    auto prevState = _transitionToState(lk, migrationId, State::kStarted);
    if (prevState == State::kStarted)
        return;

    _mh->workerThread = std::make_unique<stdx::thread>([this, migrationId] {
        Client::initThread("TenantFileImporterService");
        LOGV2_INFO(6378904,
                   "TenantFileImporterService worker thread started",
                   "migrationId"_attr = migrationId);

        {
            stdx::lock_guard<Client> lk(cc());
            cc().setSystemOperationUnkillableByStepdown(lk);
        }

        try {
            _handleEvents(migrationId);
        } catch (...) {
            LOGV2_ERROR(6615001,
                        "TenantFileImporterService::_handleEvents encountered an error",
                        "migrationId"_attr = migrationId,
                        "error"_attr = redact(exceptionToStatus()));
        }

        LOGV2_INFO(7800203,
                   "TenantFileImporterService worker thread exiting",
                   "migrationId"_attr = migrationId);
        hangBeforeFileImporterThreadExit.pauseWhileSet();
    });
}

void TenantFileImporterService::learnedFilename(const UUID& migrationId,
                                                const BSONObj& metadataDoc) {
    stdx::lock_guard lk(_mutex);
    // Migration handle can be empty only if the node restarts,rolls back, or resyncs while a shard
    // merge is in progress.
    if (!_mh) {
        LOGV2_DEBUG(7800204,
                    3,
                    "TenantFileImporterService:: Skipping learned filename",
                    "migrationId"_attr = migrationId,
                    "filename"_attr = metadataDoc["filename"]);
        return;
    }

    (void)_transitionToState(lk, migrationId, State::kLearnedFilename);
    _mh->stats.totalDataSize += std::max(0ll, metadataDoc["fileSize"].safeNumberLong());

    ImporterEvent event{ImporterEvent::Type::kLearnedFileName, migrationId};
    event.metadataDoc = metadataDoc.getOwned();
    auto success = _mh->eventQueue->tryPush(std::move(event));

    uassert(
        6378903,
        "TenantFileImporterService failed to push '{}' event without blocking for migrationId :{}"_format(
            stateToString(_mh->state), migrationId.toString()),
        success);
}

void TenantFileImporterService::learnedAllFilenames(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    // Migration handle can be empty only if the node restarts,rolls back, or resyncs while a shard
    // merge is in progress.
    if (!_mh) {
        LOGV2_DEBUG(7800205,
                    3,
                    "TenantFileImporterService:: Skipping learned all filenames",
                    "migrationId"_attr = migrationId);
        return;
    }

    auto prevState = _transitionToState(lk, migrationId, State::kLearnedAllFilenames);
    if (prevState == State::kLearnedAllFilenames)
        return;

    auto success =
        _mh->eventQueue->tryPush({ImporterEvent::Type::kLearnedAllFilenames, migrationId});
    uassert(
        6378902,
        "TenantFileImporterService failed to push '{}' event without blocking for migrationId :{}"_format(
            stateToString(_mh->state), migrationId.toString()),
        success);
}

void TenantFileImporterService::interruptMigration(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    // Migration handle can be empty only if the node restarts,rolls back, or resyncs while a shard
    // merge is in progress.
    if (!_mh) {
        LOGV2_DEBUG(7800206,
                    3,
                    "TenantFileImporterService:: Skipping interrupting migration",
                    "migrationId"_attr = migrationId);
        return;
    }
    _interrupt(lk, migrationId);
}

void TenantFileImporterService::resetMigration(const UUID& migrationId) {
    _resetMigrationHandle(migrationId);
}

void TenantFileImporterService::interruptAll() {
    stdx::lock_guard lk(_mutex);
    if (!_mh) {
        return;
    }
    _interrupt(lk, _mh->migrationId);
}

void TenantFileImporterService::_handleEvents(const UUID& migrationId) {
    auto uniqueOpCtx = cc().makeOperationContext();
    OperationContext* opCtx = uniqueOpCtx.get();

    std::unique_ptr<DBClientConnection> donorConnection;
    Queue* eventQueue;
    ThreadPool* writerPool;
    TenantMigrationSharedData* sharedData;

    ON_BLOCK_EXIT([this, opId = opCtx->getOpID(), &migrationId] {
        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);

        _mh->stats.fileCopyEnd = Date_t::now();

        _mh->opCtx = nullptr;
        _mh->donorConnection = nullptr;
    });

    {
        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        uassert(ErrorCodes::Interrupted,
                str::stream() << "TenantFileImporterService was interrupted for migrationId:\""
                              << migrationId << "\"",
                _mh->state < State::kInterrupted);
        _mh->opCtx = opCtx;

        eventQueue = _mh->eventQueue.get();
        writerPool = _mh->writerPool.get();
        sharedData = _mh->sharedData.get();
    }

    auto setUpDonorConnectionIfNeeded = [&](const BSONObj& metadataDoc) {
        // Return early if we have already set up the donor connection.
        if (donorConnection) {
            return;
        }

        donorConnection = _createConnectionFn();
        auto source = HostAndPort::parseThrowing(metadataDoc[kDonorHostNameFieldName].str());
        connect(source, donorConnection.get());

        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        uassert(ErrorCodes::Interrupted,
                str::stream() << "TenantFileImporterService was interrupted for migrationId=\""
                              << migrationId << "\"",
                _mh->state < State::kInterrupted);
        _mh->donorConnection = donorConnection.get();
    };

    using eventType = ImporterEvent::Type;
    while (true) {
        opCtx->checkForInterrupt();

        auto event = eventQueue->pop(opCtx);

        // Out-of-order events for a different migration are not permitted.
        invariant(event.migrationId == migrationId);

        switch (event.type) {
            case eventType::kNone:
                continue;
            case eventType::kLearnedFileName: {
                // We won't have valid donor metadata until the first
                // 'TenantFileImporterService::learnedFilename' call, so we need to set up the
                // connection for the first kLearnedFileName event.
                setUpDonorConnectionIfNeeded(event.metadataDoc);

                _cloneFile(opCtx,
                           migrationId,
                           donorConnection.get(),
                           writerPool,
                           sharedData,
                           event.metadataDoc);
                continue;
            }
            case eventType::kLearnedAllFilenames:
                runRollbackAndThenImportFiles(opCtx, migrationId);
                createImportDoneMarkerLocalCollection(opCtx, migrationId);
                // Take a stable checkpoint to persist both the imported donor collections and the
                // marker collection to disk.
                opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx,
                                                                         /*stableCheckpoint*/ true);
                _voteImportedFiles(opCtx, migrationId);
                return;
        }
        MONGO_UNREACHABLE;
    }
}

void TenantFileImporterService::_cloneFile(OperationContext* opCtx,
                                           const UUID& migrationId,
                                           DBClientConnection* clientConnection,
                                           ThreadPool* writerPool,
                                           TenantMigrationSharedData* sharedData,
                                           const BSONObj& metadataDoc) {
    if (MONGO_unlikely(skipCloneFiles.shouldFail())) {
        LOGV2(7800201,
              "Skipping file cloning due to 'skipCloneFiles' failpoint enabled",
              "migrationId"_attr = migrationId);
        return;
    }

    const auto fileName = metadataDoc["filename"].str();
    const auto backupId = UUID(uassertStatusOK(UUID::parse(metadataDoc[kBackupIdFieldName])));
    const auto remoteDbpath = metadataDoc["remoteDbpath"].str();
    const size_t fileSize = std::max(0ll, metadataDoc["fileSize"].safeNumberLong());
    const auto relativePath = getPathRelativeTo(fileName, metadataDoc[kDonorDbPathFieldName].str());
    LOGV2_DEBUG(6113320,
                1,
                "Cloning file",
                "migrationId"_attr = migrationId,
                "metadata"_attr = metadataDoc,
                "destinationRelativePath"_attr = relativePath);
    invariant(!relativePath.empty());

    auto currentTenantFileCloner =
        std::make_unique<TenantFileCloner>(backupId,
                                           migrationId,
                                           fileName,
                                           fileSize,
                                           relativePath,
                                           sharedData,
                                           clientConnection->getServerHostAndPort(),
                                           clientConnection,
                                           repl::StorageInterface::get(cc().getServiceContext()),
                                           writerPool);

    ON_BLOCK_EXIT([this, &migrationId] {
        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        if (_mh->currentTenantFileCloner) {
            _mh->stats.totalBytesCopied += _mh->currentTenantFileCloner->getStats().bytesCopied;
            _mh->currentTenantFileCloner = nullptr;
        }
    });

    {
        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        _mh->currentTenantFileCloner = currentTenantFileCloner.get();
    }

    auto cloneStatus = currentTenantFileCloner->run();
    uassertStatusOK(cloneStatus.withContext(str::stream()
                                            << "Failed to clone file, migrationId: " << migrationId
                                            << ", fileName: " << fileName));
}

void TenantFileImporterService::_voteImportedFiles(OperationContext* opCtx,
                                                   const UUID& migrationId) {
    if (MONGO_unlikely(hangBeforeVoteImportedFiles.shouldFail())) {
        LOGV2(7675000, "'hangBeforeVoteImportedFiles' failpoint enabled");
        hangBeforeVoteImportedFiles.pauseWhileSet();
    }

    // Build the command request.
    auto replCoord = ReplicationCoordinator::get(getGlobalServiceContext());
    RecipientVoteImportedFiles cmd(migrationId, replCoord->getMyHostAndPort());

    Backoff exponentialBackoff(Seconds(1), Milliseconds::max());

    while (true) {

        opCtx->checkForInterrupt();

        try {
            auto voteResponse = replCoord->runCmdOnPrimaryAndAwaitResponse(
                opCtx,
                DatabaseName::kAdmin,
                cmd.toBSON({}),
                [](executor::TaskExecutor::CallbackHandle handle) {},
                [](executor::TaskExecutor::CallbackHandle handle) {});

            uassertStatusOK(getStatusFromCommandResult(voteResponse));
        } catch (DBException& ex) {
            if (ErrorCodes::isNetworkError(ex)) {
                LOGV2_INFO(7675001,
                           "Retrying 'recipientVoteImportedFiles' command",
                           "retryError"_attr = redact(ex));

                // Don't hammer the network.
                opCtx->sleepFor(exponentialBackoff.nextSleep());
                continue;
            }

            ex.addContext("Failed to run 'recipientVoteImportedFiles' command");
            throw;
        }
        break;
    }
}

void TenantFileImporterService::_interrupt(WithLock lk, const UUID& migrationId) {
    auto prevState = _transitionToState(lk, migrationId, State::kInterrupted);
    if (prevState == State::kInterrupted)
        return;

    if (_mh->donorConnection) {
        _mh->donorConnection->shutdownAndDisallowReconnect();
    }

    if (_mh->writerPool) {
        _mh->writerPool->shutdown();
    }

    if (_mh->sharedData) {
        stdx::lock_guard<TenantMigrationSharedData> sharedDatalk(*_mh->sharedData);
        // Prevent the TenantFileCloner from getting retried on retryable errors.
        _mh->sharedData->setStatusIfOK(
            sharedDatalk, Status{ErrorCodes::CallbackCanceled, "TenantFileCloner canceled"});
    }

    if (_mh->eventQueue) {
        _mh->eventQueue->closeConsumerEnd();
    }

    if (_mh->opCtx) {
        stdx::lock_guard<Client> lk(*_mh->opCtx->getClient());
        _mh->opCtx->markKilled(ErrorCodes::Interrupted);
    }
}

void TenantFileImporterService::_resetMigrationHandle(boost::optional<const UUID&> migrationId) {
    stdx::unique_lock<Latch> lk(_mutex);
    _resetCV.wait(lk, [this]() { return _resetInProgress == false; });
    if (!_mh) {
        return;
    }
    if (!migrationId) {
        migrationId = _mh->migrationId;
    }

    (void)_transitionToState(lk, migrationId.value(), State::kStopped, true /*dryRun*/);
    _resetInProgress = true;

    auto workerThread = _mh->workerThread.get();
    auto writerPool = _mh->writerPool.get();
    lk.unlock();

    LOGV2(7800207,
          "TenantFileImporterService::Waiting for worker threads to join",
          "migrationId"_attr = migrationId);
    if (workerThread && workerThread->joinable()) {
        workerThread->join();
    }

    if (writerPool) {
        writerPool->join();
    }

    lk.lock();
    (void)_transitionToState(lk, migrationId.value(), State::kStopped);
    _mh.reset();

    _resetInProgress = false;
    _resetCV.notify_all();
}

TenantFileImporterService::State TenantFileImporterService::_transitionToState(
    WithLock, const UUID& migrationId, State targetState, const bool dryRun) {
    const auto isValid = [&] {
        if (!_mh || migrationId != _mh->migrationId)
            return false;

        switch (targetState) {
            case State::kUninitialized:
                return _mh->state == State::kUninitialized;
            case State::kStarted:
                return _mh->state <= State::kStarted;
            case State::kLearnedFilename:
                return _mh->state <= State::kLearnedFilename;
            case State::kLearnedAllFilenames:
                return _mh->state == State::kLearnedFilename ||
                    _mh->state == State::kLearnedAllFilenames;
            case State::kInterrupted:
                return _mh->state <= State::kInterrupted;
            case State::kStopped:
                return _mh->state == State::kUninitialized || _mh->state >= State::kInterrupted;
            default:
                MONGO_UNREACHABLE;
        }
    }();

    std::stringstream errMsg;
    errMsg << "Failed state transition check for migrationID: " << migrationId
           << ", state: " << stateToString(targetState);
    if (_mh) {
        errMsg << ", current migrationId: " << _mh->migrationId
               << ", current state: " << stateToString(_mh->state);
    }
    uassert(7800210, errMsg.str(), isValid);

    if (dryRun)
        return _mh->state;
    if (targetState != _mh->state) {
        LOGV2(7800208,
              "TenantFileImporterService:: Transitioning state to",
              "migrationId"_attr = migrationId,
              "state"_attr = stateToString(targetState));
    }
    std::swap(_mh->state, targetState);
    return targetState;
}

BSONObj TenantFileImporterService::getStats(boost::optional<const UUID&> migrationId) {
    BSONObjBuilder bob;
    getStats(bob, migrationId);
    return bob.obj();
}

void TenantFileImporterService::getStats(BSONObjBuilder& bob,
                                         boost::optional<const UUID&> migrationId) {
    stdx::lock_guard lk(_mutex);
    if (!_mh || (migrationId && migrationId.value() != _mh->migrationId))
        return;

    bob.append("approxTotalDataSize", static_cast<long long>(_mh->stats.totalDataSize));

    auto approxTotalBytesCopied = _mh->stats.totalBytesCopied;
    if (_mh->currentTenantFileCloner) {
        approxTotalBytesCopied += _mh->currentTenantFileCloner->getStats().bytesCopied;
    }
    bob.append("approxTotalBytesCopied", static_cast<long long>(approxTotalBytesCopied));

    auto fileCopyEnd = [&]() {
        return _mh->stats.fileCopyEnd == Date_t() ? Date_t::now() : _mh->stats.fileCopyEnd;
    }();
    auto elapsedMillis =
        duration_cast<Milliseconds>(fileCopyEnd - _mh->stats.fileCopyStart).count();
    bob.append("totalReceiveElapsedMillis", static_cast<long long>(elapsedMillis));


    if (approxTotalBytesCopied > _mh->stats.totalDataSize) {
        LOGV2_ERROR(7800209,
                    "TenantFileImporterService::Bytes copied is greater than actual data size",
                    "migrationId"_attr = _mh->migrationId,
                    "totalDataSize"_attr = _mh->stats.totalDataSize,
                    "totalBytesCopied"_attr = _mh->stats.totalDataSize);
    }
    int64_t timeRemainingMillis =
        ((_mh->stats.totalDataSize - approxTotalBytesCopied) * elapsedMillis) /
        (approxTotalBytesCopied + 1);
    bob.append("remainingReceiveEstimatedMillis", static_cast<long long>(timeRemainingMillis));
}

}  // namespace mongo::repl
