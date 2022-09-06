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

#include <boost/filesystem.hpp>
#include <fmt/format.h>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_import.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo::repl {

using namespace fmt::literals;
using namespace shard_merge_utils;
using namespace tenant_migration_access_blocker;

namespace {
const auto _TenantFileImporterService =
    ServiceContext::declareDecoration<TenantFileImporterService>();

const ReplicaSetAwareServiceRegistry::Registerer<TenantFileImporterService>
    _TenantFileImporterServiceRegisterer("TenantFileImporterService");

/**
 * Makes a connection to the provided 'source'.
 */
Status connect(const HostAndPort& source, DBClientConnection* client) {
    Status status = client->connect(source, "TenantFileImporterService", boost::none);
    if (!status.isOK())
        return status;
    return replAuthenticate(client).withContext(str::stream()
                                                << "Failed to authenticate to " << source);
}

void importCopiedFiles(OperationContext* opCtx, const UUID& migrationId) {
    auto tempWTDirectory = fileClonerTempDir(migrationId);
    uassert(6113315,
            str::stream() << "Missing file cloner's temporary dbpath directory: "
                          << tempWTDirectory.string(),
            boost::filesystem::exists(tempWTDirectory));

    // TODO SERVER-63204: Evaluate correct place to remove the temporary WT dbpath.
    ON_BLOCK_EXIT([&tempWTDirectory, &migrationId] {
        LOGV2_INFO(6113324,
                   "Done importing files, removing the temporary WT dbpath",
                   "migrationId"_attr = migrationId,
                   "tempDbPath"_attr = tempWTDirectory.string());
        boost::system::error_code ec;
        boost::filesystem::remove_all(tempWTDirectory, ec);
    });

    auto metadatas = wiredTigerRollbackToStableAndGetMetadata(opCtx, tempWTDirectory.string());
    for (auto&& m : metadatas) {
        auto tenantId = parseTenantIdFromDB(m.ns.db());
        if (tenantId == boost::none) {
            continue;
        }

        LOGV2_DEBUG(6114100, 1, "Create recipient access blocker", "tenantId"_attr = tenantId);
        addTenantMigrationRecipientAccessBlocker(
            opCtx->getServiceContext(), *tenantId, migrationId);
    }

    wiredTigerImportFromBackupCursor(opCtx, metadatas, tempWTDirectory.string());

    auto catalog = CollectionCatalog::get(opCtx);
    for (auto&& m : metadatas) {
        AutoGetDb dbLock(opCtx, m.ns.dbName(), MODE_IX);
        Lock::CollectionLock systemViewsLock(
            opCtx,
            NamespaceString(m.ns.dbName(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);
        uassertStatusOK(catalog->reloadViews(opCtx, m.ns.dbName()));
    }
}
}  // namespace

TenantFileImporterService* TenantFileImporterService::get(ServiceContext* serviceContext) {
    return &_TenantFileImporterService(serviceContext);
}

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
        try {
            _handleEvents(migrationId);
        } catch (const DBException& err) {
            LOGV2_ERROR(6615001,
                        "TenantFileImporterService::_handleEvents encountered an error",
                        "error"_attr = err.toString());
        }
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
        LOGV2_WARNING(
            6378901,
            "Called interrupt with migrationId {migrationId}, but {activeMigrationId} is active",
            "migrationId"_attr = migrationId.toString(),
            "activeMigrationId"_attr = _migrationId ? _migrationId->toString() : "no migration");
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
    auto opCtx = cc().makeOperationContext();

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
        _opCtx = opCtx.get();
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

    auto setUpImporterResourcesIfNeeded = [&](const BSONObj& metadataDoc) {
        // Return early if we have already set up the donor connection.
        if (donorConnection) {
            return;
        }

        auto conn = std::make_shared<DBClientConnection>(true /* autoReconnect */);
        auto donor = HostAndPort::parseThrowing(metadataDoc[kDonorFieldName].str());
        uassertStatusOK(connect(donor, conn.get()));

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

        auto event = eventQueue->pop(opCtx.get());

        // Out-of-order events for a different migration are not permitted.
        invariant(event.migrationId == migrationId);

        switch (event.type) {
            case eventType::kNone:
                continue;
            case eventType::kLearnedFileName: {
                // we won't have valid donor metadata until the first
                // 'TenantFileImporterService::learnedFilename' call, so we need to set up the
                // connection for the first kLearnedFileName event.
                setUpImporterResourcesIfNeeded(event.metadataDoc);

                cloneFile(opCtx.get(),
                          donorConnection.get(),
                          writerPool.get(),
                          sharedData.get(),
                          event.metadataDoc);
                continue;
            }
            case eventType::kLearnedAllFilenames:
                importCopiedFiles(opCtx.get(), migrationId);
                _voteImportedFiles(opCtx.get(), migrationId);
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
        NamespaceString::kAdminDb.toString(),
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
