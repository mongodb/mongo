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

namespace mongo::repl {

using namespace fmt::literals;

namespace {
const auto _TenantFileImporterService =
    ServiceContext::declareDecoration<TenantFileImporterService>();

const ReplicaSetAwareServiceRegistry::Registerer<TenantFileImporterService>
    _TenantFileImporterServiceRegisterer("TenantFileImporterService");

/**
 * Connect to the donor source.
 */
void connect(const BSONObj& metadataDoc, DBClientConnection* client) {
    auto source = HostAndPort::parseThrowing(metadataDoc[shard_merge_utils::kDonorFieldName].str());
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
    : _importFiles(shard_merge_utils::wiredTigerImport), _createConnection([]() {
          return std::make_shared<DBClientConnection>(true /* autoReconnect */);
      }) {}

void TenantFileImporterService::startMigration(const UUID& migrationId) {
    _reset();

    stdx::lock_guard lk(_mutex);
    if (_isShuttingDown || _state != State::kUninitialized) {
        LOGV2_DEBUG(6690701,
                    1,
                    "TenantFileImporterService cannot start a new migration, an existing migration "
                    "is active or we are shutting down",
                    "state"_attr = stateToString(_state),
                    "migrationId"_attr = _migrationId ? _migrationId->toString() : "(empty)");
        return;
    }

    // When state is kUninitialized, we expect _migrationId to be empty.
    invariant(!_migrationId, str::stream() << "migrationId: " << _migrationId->toString());

    _migrationId = migrationId;
    _eventQueue = std::make_shared<Queue>();
    _state = State::kStarted;

    _workerThread = std::make_unique<stdx::thread>([this, migrationId] {
        Client::initThread("TenantFileImporterService");

        // TODO(SERVER-74661): Please revisit if this thread could be made killable.
        {
            stdx::lock_guard<Client> lk(cc());
            cc().setSystemOperationUnkillableByStepdown(lk);
        }

        try {
            _handleEvents(migrationId);
        } catch (const DBException& err) {
            LOGV2_ERROR(6615001,
                        "TenantFileImporterService::_handleEvents encountered an error",
                        "error"_attr = err.toString());
        }

        hangBeforeFileImporterThreadExit.pauseWhileSet();
    });
}

void TenantFileImporterService::learnedFilename(const UUID& migrationId,
                                                const BSONObj& metadataDoc) {
    stdx::lock_guard lk(_mutex);
    if (migrationId == _migrationId && _state >= State::kLearnedAllFilenames) {
        return;
    }

    tassert(8423347,
            "Called learnedFilename with migrationId {}, but {} is active"_format(
                migrationId.toString(), _migrationId ? _migrationId->toString() : "no migration"),
            migrationId == _migrationId);

    _state = State::kLearnedFilename;
    ImporterEvent event{ImporterEvent::Type::kLearnedFileName, migrationId};
    event.metadataDoc = metadataDoc.getOwned();
    invariant(_eventQueue);
    auto success = _eventQueue->tryPush(std::move(event));

    uassert(6378903,
            "TenantFileImporterService failed to push '{}' event without blocking"_format(
                stateToString(_state)),
            success);
}

void TenantFileImporterService::learnedAllFilenames(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    if (migrationId == _migrationId && _state >= State::kLearnedAllFilenames) {
        return;
    }

    tassert(8423345,
            "Called learnedAllFilenames with migrationId {}, but {} is active"_format(
                migrationId.toString(), _migrationId ? _migrationId->toString() : "no migration"),
            migrationId == _migrationId);

    _state = State::kLearnedAllFilenames;
    invariant(_eventQueue);
    auto success = _eventQueue->tryPush({ImporterEvent::Type::kLearnedAllFilenames, migrationId});
    uassert(6378902,
            "TenantFileImporterService failed to push '{}' event without blocking"_format(
                stateToString(_state)),
            success);
}

void TenantFileImporterService::interrupt(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    if (migrationId != _migrationId) {
        LOGV2_WARNING(6378901,
                      "TenantFileImporterService interrupted",
                      "migrationId"_attr = migrationId.toString(),
                      "activeMigrationId"_attr =
                          _migrationId ? _migrationId->toString() : "no migration");
        return;
    }
    _interrupt(lk);
}

void TenantFileImporterService::interruptAll() {
    stdx::lock_guard lk(_mutex);
    if (!_migrationId) {
        return;
    }
    _interrupt(lk);
}

void TenantFileImporterService::_handleEvents(const UUID& migrationId) {
    auto uniqueOpCtx = cc().makeOperationContext();
    OperationContext* opCtx = uniqueOpCtx.get();

    ON_BLOCK_EXIT([this, opId = opCtx->getOpID()] {
        stdx::lock_guard lk(_mutex);
        if (_opCtx && _opCtx->getOpID() == opId) {
            _opCtx = nullptr;
        }
    });

    {
        stdx::lock_guard lk(_mutex);
        uassert(ErrorCodes::Interrupted,
                str::stream() << "TenantFileImporterService was interrupted for migrationId=\""
                              << _migrationId << "\"",
                migrationId == _migrationId && _state != State::kInterrupted);
        _opCtx = opCtx;
    }

    LOGV2_INFO(6378904,
               "TenantFileImporterService starting worker thread",
               "migrationId"_attr = migrationId.toString());

    std::shared_ptr<Queue> eventQueue;
    {
        stdx::lock_guard lk(_mutex);
        invariant(_eventQueue);
        eventQueue = _eventQueue;
    }

    std::shared_ptr<DBClientConnection> donorConnection;
    std::shared_ptr<ThreadPool> writerPool;
    std::shared_ptr<TenantMigrationSharedData> sharedData;

    auto setUpDonorConnectionIfNeeded = [&](const BSONObj& metadataDoc) {
        // Return early if we have already set up the donor connection.
        if (donorConnection) {
            return;
        }

        auto conn = _createConnection();
        connect(metadataDoc, conn.get());

        stdx::lock_guard lk(_mutex);
        uassert(ErrorCodes::Interrupted,
                str::stream() << "TenantFileImporterService was interrupted for migrationId=\""
                              << _migrationId << "\"",
                migrationId == _migrationId && _state != State::kInterrupted);

        _donorConnection = std::move(conn);
        _writerPool =
            makeReplWriterPool(tenantApplierThreadCount, "TenantFileImporterServiceWriter"_sd);
        _sharedData = std::make_shared<TenantMigrationSharedData>(
            getGlobalServiceContext()->getFastClockSource(), _migrationId.get());
        donorConnection = _donorConnection;
        writerPool = _writerPool;
        sharedData = _sharedData;
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

                shard_merge_utils::cloneFile(opCtx,
                                             donorConnection.get(),
                                             writerPool.get(),
                                             sharedData.get(),
                                             event.metadataDoc);
                continue;
            }
            case eventType::kLearnedAllFilenames:
                _importFiles(opCtx, migrationId);
                // Take a stable checkpoint so that all the imported donor & marker collection
                // metadata infos are persisted to disk.
                opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx,
                                                                         /*stableCheckpoint*/ true);
                _voteImportedFiles(opCtx, migrationId);
                break;
        }
        break;
    }
}

void TenantFileImporterService::_voteImportedFiles(OperationContext* opCtx,
                                                   const UUID& migrationId) {
    auto replCoord = ReplicationCoordinator::get(getGlobalServiceContext());

    RecipientVoteImportedFiles cmd(migrationId, replCoord->getMyHostAndPort(), true /* success */);

    auto voteResponse = replCoord->runCmdOnPrimaryAndAwaitResponse(
        opCtx,
        DatabaseName::kAdmin.db().toString(),
        cmd.toBSON({}),
        [](executor::TaskExecutor::CallbackHandle handle) {},
        [](executor::TaskExecutor::CallbackHandle handle) {});

    auto voteStatus = getStatusFromCommandResult(voteResponse);
    if (!voteStatus.isOK()) {
        LOGV2_WARNING(6113403,
                      "Failed to run recipientVoteImportedFiles command on primary",
                      "status"_attr = voteStatus);
        // TODO SERVER-64192: handle this case, retry, and/or throw error, etc.
    }
}

void TenantFileImporterService::_interrupt(WithLock) {
    if (_state == State::kInterrupted) {
        return;
    }

    if (_donorConnection) {
        _donorConnection->shutdownAndDisallowReconnect();
    }

    if (_writerPool) {
        _writerPool->shutdown();
    }

    if (_sharedData) {
        stdx::lock_guard<TenantMigrationSharedData> sharedDatalk(*_sharedData);
        // Prevent the TenantFileCloner from getting retried on retryable errors.
        _sharedData->setStatusIfOK(
            sharedDatalk, Status{ErrorCodes::CallbackCanceled, "TenantFileCloner canceled"});
    }

    if (_eventQueue) {
        _eventQueue->closeConsumerEnd();
    }

    if (_opCtx) {
        stdx::lock_guard<Client> lk(*_opCtx->getClient());
        _opCtx->markKilled(ErrorCodes::Interrupted);
    }

    _state = State::kInterrupted;
}

void TenantFileImporterService::_reset() {
    std::unique_ptr<stdx::thread> workerThread = nullptr;
    std::shared_ptr<ThreadPool> writerPool = nullptr;
    {
        stdx::lock_guard lk(_mutex);
        if (!_migrationId) {
            invariant(_state == State::kUninitialized,
                      str::stream() << "current state: " << stateToString(_state));
            return;
        }

        if (_state != State::kInterrupted) {
            LOGV2_DEBUG(6690700,
                        1,
                        "TenantFileImporterService cannot be reset until the current migration has "
                        "been interrupted",
                        "migrationId"_attr = _migrationId->toString());
            return;
        }

        _state = State::kUninitialized;

        LOGV2_INFO(6378905,
                   "TenantFileImporterService resetting migration",
                   "migrationId"_attr = _migrationId->toString());
        _migrationId.reset();

        std::swap(workerThread, _workerThread);
        std::swap(writerPool, _writerPool);
    }

    if (workerThread && workerThread->joinable()) {
        workerThread->join();
    }

    if (writerPool) {
        writerPool->join();
    }
}
}  // namespace mongo::repl
