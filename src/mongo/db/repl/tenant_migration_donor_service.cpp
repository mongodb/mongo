/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/db/repl/tenant_migration_donor_service.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/tenant_migration_access_blocker.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_donor_util.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(abortTenantMigrationAfterBlockingStarts);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationAfterBlockingStarts);
MONGO_FAIL_POINT_DEFINE(skipSendingRecipientSyncDataCommand);

const Seconds kRecipientSyncDataTimeout(30);

}  // namespace

TenantMigrationDonorService::Instance::Instance(ServiceContext* serviceContext,
                                                const BSONObj& initialState)
    : repl::PrimaryOnlyService::TypedInstance<Instance>(), _serviceContext(serviceContext) {
    _stateDoc =
        TenantMigrationDonorDocument::parse(IDLParserErrorContext("initialStateDoc"), initialState);

    _mtab = std::make_shared<TenantMigrationAccessBlocker>(
        _serviceContext,
        tenant_migration_donor::makeTenantMigrationExecutor(_serviceContext),
        _stateDoc.getDatabasePrefix().toString());
    TenantMigrationAccessBlockerByPrefix::get(_serviceContext)
        .add(_stateDoc.getDatabasePrefix(), _mtab);
}

void TenantMigrationDonorService::Instance::_insertStateDocument(OperationContext* opCtx) {
    PersistentTaskStore<TenantMigrationDonorDocument> store(_stateDocumentsNS);
    try {
        store.add(opCtx, _stateDoc);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        uasserted(
            4917300,
            str::stream()
                << "While attempting to persist the donor's state machine for tenant migration"
                << ", found another document with the same migration id. Attempted migration: "
                << _stateDoc.toBSON());
    }
}

void TenantMigrationDonorService::Instance::_updateStateDocument(
    OperationContext* opCtx, const TenantMigrationDonorStateEnum nextState) {
    uassertStatusOK(
        writeConflictRetry(opCtx, "updateStateDoc", _stateDocumentsNS.ns(), [&]() -> Status {
            AutoGetCollection autoCollection(opCtx, _stateDocumentsNS, MODE_IX);
            Collection* collection = autoCollection.getCollection();

            if (!collection) {
                return Status(ErrorCodes::NamespaceNotFound,
                              str::stream() << _stateDocumentsNS.ns() << " does not exist");
            }
            WriteUnitOfWork wuow(opCtx);

            const auto originalstateDoc = _stateDoc.toBSON();
            const auto originalRecordId =
                Helpers::findOne(opCtx, collection, originalstateDoc, false /* requireIndex */);
            const auto originalSnapshot =
                Snapshotted<BSONObj>(opCtx->recoveryUnit()->getSnapshotId(), originalstateDoc);
            invariant(!originalRecordId.isNull());

            // Reserve an opTime for the write.
            auto oplogSlot = repl::LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];

            // Update the state.
            _stateDoc.setState(nextState);
            switch (nextState) {
                case TenantMigrationDonorStateEnum::kBlocking:
                    _stateDoc.setBlockTimestamp(oplogSlot.getTimestamp());
                    break;
                case TenantMigrationDonorStateEnum::kCommitted:
                case TenantMigrationDonorStateEnum::kAborted:
                    _stateDoc.setCommitOrAbortOpTime(oplogSlot);
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
            const auto updatedstateDoc = _stateDoc.toBSON();

            CollectionUpdateArgs args;
            args.criteria = BSON("_id" << _stateDoc.getId());
            args.oplogSlot = oplogSlot;
            args.update = updatedstateDoc;

            collection->updateDocument(opCtx,
                                       originalRecordId,
                                       originalSnapshot,
                                       updatedstateDoc,
                                       false,
                                       nullptr /* OpDebug* */,
                                       &args);
            wuow.commit();
            return Status::OK();
        }));
}

void TenantMigrationDonorService::Instance::_sendRecipientSyncDataCommand(
    OperationContext* opCtx,
    executor::TaskExecutor* executor,
    RemoteCommandTargeter* recipientTargeter) {
    if (skipSendingRecipientSyncDataCommand.shouldFail()) {
        return;
    }

    BSONObj cmdObj = BSONObj([&]() {
        auto donorConnString =
            repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString();
        RecipientSyncData request(_stateDoc.getId(),
                                  donorConnString.toString(),
                                  _stateDoc.getDatabasePrefix().toString(),
                                  _stateDoc.getReadPreference());
        request.setReturnAfterReachingOpTime(_stateDoc.getBlockTimestamp());
        return request.toBSON(BSONObj());
    }());

    HostAndPort recipientHost =
        uassertStatusOK(recipientTargeter->findHost(opCtx, ReadPreferenceSetting()));

    executor::RemoteCommandRequest request(recipientHost,
                                           NamespaceString::kAdminDb.toString(),
                                           std::move(cmdObj),
                                           rpc::makeEmptyMetadata(),
                                           nullptr,
                                           kRecipientSyncDataTimeout);

    executor::RemoteCommandResponse response =
        Status(ErrorCodes::InternalError, "Internal error running command");

    executor::TaskExecutor::CallbackHandle cbHandle =
        uassertStatusOK(executor->scheduleRemoteCommand(
            request, [&response](const auto& args) { response = args.response; }));

    executor->wait(cbHandle, opCtx);
    uassertStatusOK(getStatusFromCommandResult(response.data));
}

Status TenantMigrationDonorService::Instance::checkIfOptionsConflict(BSONObj options) {
    auto stateDoc = TenantMigrationDonorDocument::parse(IDLParserErrorContext("stateDoc"), options);

    if (stateDoc.getId() != _stateDoc.getId() ||
        stateDoc.getDatabasePrefix() != _stateDoc.getDatabasePrefix() ||
        stateDoc.getRecipientConnectionString() != _stateDoc.getRecipientConnectionString() ||
        SimpleBSONObjComparator::kInstance.compare(stateDoc.getReadPreference().toInnerBSON(),
                                                   _stateDoc.getReadPreference().toInnerBSON()) !=
            0) {
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      str::stream() << "Found active migration for dbPrefix \""
                                    << stateDoc.getDatabasePrefix() << "\" with different options "
                                    << _stateDoc.toBSON());
    }

    return Status::OK();
}

void TenantMigrationDonorService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    ExecutorFuture<void>(**executor)
        .then([this, executor] {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto recipientUri = uassertStatusOK(
                MongoURI::parse(_stateDoc.getRecipientConnectionString().toString()));
            auto recipientTargeter = std::make_unique<RemoteCommandTargeterRS>(
                recipientUri.getSetName(), recipientUri.getServers());
            auto removeRecipientReplicaSetMonitorGuard =
                makeGuard([&] { ReplicaSetMonitor::remove(recipientUri.getSetName()); });

            // Enter "dataSync" state.
            invariant(_stateDoc.getState() == TenantMigrationDonorStateEnum::kDataSync);
            _insertStateDocument(opCtx);
            _sendRecipientSyncDataCommand(opCtx, *executor, recipientTargeter.get());

            // Enter "blocking" state.
            _mtab->startBlockingWrites();
            // TODO (SERVER-50439): Make TenantMigrationDonorService wait for the write to state doc
            // to be majority committed.
            _updateStateDocument(opCtx, TenantMigrationDonorStateEnum::kBlocking);
            _sendRecipientSyncDataCommand(opCtx, *executor, recipientTargeter.get());

            pauseTenantMigrationAfterBlockingStarts.pauseWhileSet(opCtx);

            abortTenantMigrationAfterBlockingStarts.execute([&](const BSONObj& data) {
                if (data.hasField("blockTimeMS")) {
                    const auto blockTime = Milliseconds{data.getIntField("blockTimeMS")};
                    LOGV2(5010400,
                          "Keep migration in blocking state before aborting",
                          "blockTime"_attr = blockTime);
                    opCtx->sleepFor(blockTime);
                }

                uasserted(ErrorCodes::InternalError, "simulate a tenant migration error");
            });

            // Enter "commit" state.
            _updateStateDocument(opCtx, TenantMigrationDonorStateEnum::kCommitted);
        })
        .onError([this](Status status) {
            // Enter "abort" state.
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();
            _abortReason.emplace(status);
            _updateStateDocument(opCtx, TenantMigrationDonorStateEnum::kAborted);
        })
        .then([this] {
            // Wait for the migration to commit or abort.
            return _mtab->onCompletion();
        })
        .getAsync([this](Status status) {
            if (!status.isOK()) {
                if (_abortReason) {
                    status.addContext(str::stream()
                                      << "Tenant migration with id \"" << _stateDoc.getId()
                                      << "\" and dbPrefix \"" << _stateDoc.getDatabasePrefix()
                                      << "\" aborted due to " << _abortReason);
                }
                _completionPromise.setError(status);
            } else {
                _completionPromise.emplaceValue();
            }

            LOGV2(5006601,
                  "Tenant migration completed",
                  "migrationId"_attr = _stateDoc.getId(),
                  "dbPrefix"_attr = _stateDoc.getDatabasePrefix(),
                  "status"_attr = status,
                  "abortReason"_attr = _abortReason);
        });
}

}  // namespace mongo
