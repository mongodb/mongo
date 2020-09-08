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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/db/repl/tenant_migration_donor_service.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_migration_access_blocker.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_donor_util.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(abortTenantMigrationAfterBlockingStarts);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationAfterBlockingStarts);
MONGO_FAIL_POINT_DEFINE(skipSendingRecipientSyncDataCommand);
MONGO_FAIL_POINT_DEFINE(disallowTenantMigrations);

const Seconds kRecipientSyncDataTimeout(30);

std::shared_ptr<TenantMigrationAccessBlocker> getTenantMigrationAccessBlocker(
    ServiceContext* serviceContext, StringData dbPrefix) {
    return TenantMigrationAccessBlockerByPrefix::get(serviceContext)
        .getTenantMigrationAccessBlockerForDbPrefix(dbPrefix);
}

bool shouldStopInsertingDonorStateDoc(Status status) {
    return status.isOK() || status == ErrorCodes::ConflictingOperationInProgress ||
        ErrorCodes::isShutdownError(status) || ErrorCodes::isNotPrimaryError(status);
}

bool shouldStopUpdatingDonorStateDoc(Status status) {
    return status.isOK() || ErrorCodes::isShutdownError(status) ||
        ErrorCodes::isNotPrimaryError(status);
}

bool shouldStopSendingRecipientCommand(Status status) {
    return status.isOK() || ErrorCodes::isShutdownError(status) ||
        ErrorCodes::isNotPrimaryError(status);
}

}  // namespace

TenantMigrationDonorService::Instance::Instance(ServiceContext* serviceContext,
                                                const BSONObj& initialState)
    : repl::PrimaryOnlyService::TypedInstance<Instance>(), _serviceContext(serviceContext) {
    _stateDoc =
        TenantMigrationDonorDocument::parse(IDLParserErrorContext("initialStateDoc"), initialState);
}

TenantMigrationDonorService::Instance::~Instance() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_decisionPromise.getFuture().isReady());
    invariant(_receiveDonorForgetMigrationPromise.getFuture().isReady());
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

void TenantMigrationDonorService::Instance::onReceiveDonorForgetMigration() {
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_receiveDonorForgetMigrationPromise.getFuture().isReady()) {
        _receiveDonorForgetMigrationPromise.emplaceValue();
    }
}

void TenantMigrationDonorService::Instance::interrupt(Status status) {
    stdx::lock_guard<Latch> lg(_mutex);
    // Resolve any unresolved promises to avoid hanging.
    if (!_decisionPromise.getFuture().isReady()) {
        _decisionPromise.setError(status);
    }
    if (!_receiveDonorForgetMigrationPromise.getFuture().isReady()) {
        _receiveDonorForgetMigrationPromise.setError(status);
    }
}

ExecutorFuture<repl::OpTime> TenantMigrationDonorService::Instance::_insertStateDocument(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    invariant(_stateDoc.getState() == TenantMigrationDonorStateEnum::kUninitialized);
    _stateDoc.setState(TenantMigrationDonorStateEnum::kDataSync);

    return AsyncTry([this] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient dbClient(opCtx);

               auto commandResponse = dbClient.runCommand([&] {
                   write_ops::Update updateOp(_stateDocumentsNS);
                   auto updateModification =
                       write_ops::UpdateModification::parseFromClassicUpdate(_stateDoc.toBSON());
                   write_ops::UpdateOpEntry updateEntry(
                       BSON(TenantMigrationDonorDocument::kIdFieldName << _stateDoc.getId()),
                       updateModification);
                   updateEntry.setMulti(false);
                   updateEntry.setUpsert(true);
                   updateOp.setUpdates({updateEntry});

                   return updateOp.serialize({});
               }());

               const auto commandReply = commandResponse->getCommandReply();
               uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopInsertingDonorStateDoc(swOpTime.getStatus());
        })
        .on(**executor);
}

ExecutorFuture<repl::OpTime> TenantMigrationDonorService::Instance::_updateStateDocument(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const TenantMigrationDonorStateEnum nextState) {
    const auto originalStateDocBson = _stateDoc.toBSON();

    return AsyncTry([this, executor, nextState, originalStateDocBson] {
               boost::optional<repl::OpTime> updateOpTime;

               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               uassertStatusOK(writeConflictRetry(
                   opCtx, "updateStateDoc", _stateDocumentsNS.ns(), [&]() -> Status {
                       AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);
                       if (!collection) {
                           return Status(ErrorCodes::NamespaceNotFound,
                                         str::stream()
                                             << _stateDocumentsNS.ns() << " does not exist");
                       }

                       WriteUnitOfWork wuow(opCtx);

                       const auto originalRecordId = Helpers::findOne(opCtx,
                                                                      collection.getCollection(),
                                                                      originalStateDocBson,
                                                                      false /* requireIndex */);
                       const auto originalSnapshot = Snapshotted<BSONObj>(
                           opCtx->recoveryUnit()->getSnapshotId(), originalStateDocBson);
                       invariant(!originalRecordId.isNull());

                       // Reserve an opTime for the write.
                       auto oplogSlot =
                           repl::LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];

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
                       const auto updatedStateDocBson = _stateDoc.toBSON();

                       CollectionUpdateArgs args;
                       args.criteria = BSON("_id" << _stateDoc.getId());
                       args.oplogSlot = oplogSlot;
                       args.update = updatedStateDocBson;

                       collection->updateDocument(opCtx,
                                                  originalRecordId,
                                                  originalSnapshot,
                                                  updatedStateDocBson,
                                                  false,
                                                  nullptr /* OpDebug* */,
                                                  &args);

                       wuow.commit();

                       updateOpTime = oplogSlot;
                       return Status::OK();
                   }));

               invariant(updateOpTime);
               return updateOpTime.get();
           })
        .until([](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopUpdatingDonorStateDoc(swOpTime.getStatus());
        })
        .on(**executor);
}

ExecutorFuture<repl::OpTime>
TenantMigrationDonorService::Instance::_markStateDocumentAsGarbageCollectable(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    return AsyncTry([this] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient dbClient(opCtx);

               _stateDoc.setExpireAt(
                   _serviceContext->getFastClockSource()->now() +
                   Milliseconds{repl::tenantMigrationGarbageCollectionDelayMS.load()});

               auto commandResponse = dbClient.runCommand([&] {
                   write_ops::Update updateOp(_stateDocumentsNS);
                   auto updateModification =
                       write_ops::UpdateModification::parseFromClassicUpdate(_stateDoc.toBSON());
                   write_ops::UpdateOpEntry updateEntry(
                       BSON(TenantMigrationDonorDocument::kIdFieldName << _stateDoc.getId()),
                       updateModification);
                   updateEntry.setMulti(false);
                   updateEntry.setUpsert(false);
                   updateOp.setUpdates({updateEntry});

                   return updateOp.serialize({});
               }());

               const auto commandReply = commandResponse->getCommandReply();
               uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopUpdatingDonorStateDoc(swOpTime.getStatus());
        })
        .on(**executor);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_waitForMajorityWriteConcern(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, repl::OpTime opTime) {
    return WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(std::move(opTime))
        .thenRunOn(**executor);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendCommandToRecipient(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const BSONObj& cmdObj) {
    return AsyncTry([this, executor, recipientTargeterRS, cmdObj] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               HostAndPort recipientHost =
                   uassertStatusOK(recipientTargeterRS->findHost(opCtx, ReadPreferenceSetting()));

               executor::RemoteCommandRequest request(recipientHost,
                                                      NamespaceString::kAdminDb.toString(),
                                                      std::move(cmdObj),
                                                      rpc::makeEmptyMetadata(),
                                                      nullptr,
                                                      kRecipientSyncDataTimeout);

               auto recipientSyncDataResponsePF =
                   makePromiseFuture<executor::TaskExecutor::RemoteCommandCallbackArgs>();
               auto promisePtr =
                   std::make_shared<Promise<executor::TaskExecutor::RemoteCommandCallbackArgs>>(
                       std::move(recipientSyncDataResponsePF.promise));

               auto scheduleResult =
                   (**executor)
                       ->scheduleRemoteCommand(std::move(request), [promisePtr](const auto& args) {
                           promisePtr->emplaceValue(args);
                       });

               if (!scheduleResult.isOK()) {
                   // Since the command failed to be scheduled, the callback above did not and will
                   // not run. Thus, it is safe to fulfill the promise here without worrying about
                   // synchronizing access with the executor's thread.
                   promisePtr->setError(scheduleResult.getStatus());
               }

               return std::move(recipientSyncDataResponsePF.future)
                   .thenRunOn(**executor)
                   .then([this](auto args) -> Status {
                       if (!args.response.status.isOK()) {
                           return args.response.status;
                       }
                       return getStatusFromCommandResult(args.response.data);
                   });
           })
        .until([](Status status) { return shouldStopSendingRecipientCommand(status); })
        .on(**executor);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendRecipientSyncDataCommand(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS) {
    if (skipSendingRecipientSyncDataCommand.shouldFail()) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

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

    return _sendCommandToRecipient(executor, recipientTargeterRS, cmdObj);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendRecipientForgetMigrationCommand(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS) {
    return _sendCommandToRecipient(executor,
                                   recipientTargeterRS,
                                   RecipientForgetMigration(_stateDoc.getId()).toBSON(BSONObj()));
}

SemiFuture<void> TenantMigrationDonorService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {

    // TODO SERVER-50466 Remove this failpoint and ensure that migrations resume on step-up after
    // startup recovery.
    if (MONGO_unlikely(disallowTenantMigrations.shouldFail())) {
        return ExecutorFuture<void>(**executor, Status::OK()).semi();
    }
    auto recipientUri =
        uassertStatusOK(MongoURI::parse(_stateDoc.getRecipientConnectionString().toString()));
    auto recipientTargeterRS = std::shared_ptr<RemoteCommandTargeterRS>(
        new RemoteCommandTargeterRS(recipientUri.getSetName(), recipientUri.getServers()),
        [this, setName = recipientUri.getSetName()](RemoteCommandTargeterRS* p) {
            ReplicaSetMonitor::remove(setName);
            delete p;
        });

    return ExecutorFuture<void>(**executor)
        .then([this, executor] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kUninitialized) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            // Enter "dataSync" state.
            return _insertStateDocument(executor).then([this, executor](repl::OpTime opTime) {
                return _waitForMajorityWriteConcern(executor, std::move(opTime));
            });
        })
        .then([this, executor, recipientTargeterRS] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kDataSync) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            return _sendRecipientSyncDataCommand(executor, recipientTargeterRS)
                .then([this, executor] {
                    // Enter "blocking" state.
                    auto mtab = getTenantMigrationAccessBlocker(_serviceContext,
                                                                _stateDoc.getDatabasePrefix());
                    invariant(mtab);
                    mtab->startBlockingWrites();
                    return _updateStateDocument(executor, TenantMigrationDonorStateEnum::kBlocking)
                        .then([this, executor](repl::OpTime opTime) {
                            return _waitForMajorityWriteConcern(executor, std::move(opTime));
                        });
                });
        })
        .then([this, executor, recipientTargeterRS] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kBlocking) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            invariant(_stateDoc.getBlockTimestamp());

            return _sendRecipientSyncDataCommand(executor, recipientTargeterRS)
                .then([this] {
                    auto opCtxHolder = cc().makeOperationContext();
                    auto opCtx = opCtxHolder.get();

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
                })
                .then([this, executor] {
                    // Enter "commit" state.
                    return _updateStateDocument(executor, TenantMigrationDonorStateEnum::kCommitted)
                        .then([this, executor](repl::OpTime opTime) {
                            return _waitForMajorityWriteConcern(executor, std::move(opTime));
                        });
                });
        })
        .onError([this, executor](Status status) {
            if (_stateDoc.getState() == TenantMigrationDonorStateEnum::kAborted) {
                // The migration was resumed on startup and it was already aborted.
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            auto mtab =
                getTenantMigrationAccessBlocker(_serviceContext, _stateDoc.getDatabasePrefix());
            if (status == ErrorCodes::ConflictingOperationInProgress || !mtab) {
                return ExecutorFuture<void>(**executor, status);
            } else {
                // Enter "abort" state.
                _abortReason.emplace(status);
                return _updateStateDocument(executor, TenantMigrationDonorStateEnum::kAborted)
                    .then([this, executor](repl::OpTime opTime) {
                        return _waitForMajorityWriteConcern(executor, std::move(opTime));
                    });
            }
        })
        .onCompletion([this](Status status) {
            LOGV2(5006601,
                  "Tenant migration completed",
                  "migrationId"_attr = _stateDoc.getId(),
                  "dbPrefix"_attr = _stateDoc.getDatabasePrefix(),
                  "status"_attr = status,
                  "abortReason"_attr = _abortReason);

            stdx::lock_guard<Latch> lg(_mutex);

            if (_decisionPromise.getFuture().isReady()) {
                // The promise has already been fulfilled due to stepup or shutdown.
                return;
            }

            if (status.isOK()) {
                // The migration commited or aborted successfully.
                if (_abortReason) {
                    _decisionPromise.setError(
                        {ErrorCodes::TenantMigrationAborted,
                         str::stream() << "Tenant migration with id \"" << _stateDoc.getId()
                                       << "\" and dbPrefix \"" << _stateDoc.getDatabasePrefix()
                                       << "\" aborted due to " << _abortReason});
                } else {
                    _decisionPromise.emplaceValue();
                }
            } else {
                // There was a conflicting migration or this node is stepping down or shutting down.
                _decisionPromise.setError(status);
            }
        })
        .then([this, executor, recipientTargeterRS] {
            if (_stateDoc.getExpireAt()) {
                // The migration state has already been marked as garbage collectable. Set the
                // donorForgetMigration promise here since the Instance's destructor has an
                // invariant that _receiveDonorForgetMigrationPromise is ready.
                onReceiveDonorForgetMigration();
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            // Wait for the donorForgetMigration command.
            return std::move(_receiveDonorForgetMigrationPromise.getFuture())
                .thenRunOn(**executor)
                .then([this, executor, recipientTargeterRS] {
                    return _sendRecipientForgetMigrationCommand(executor, recipientTargeterRS);
                })
                .then([this, executor] { return _markStateDocumentAsGarbageCollectable(executor); })
                .then([this, executor](repl::OpTime opTime) {
                    return _waitForMajorityWriteConcern(executor, std::move(opTime));
                });
        })
        .onCompletion([this](Status status) {
            LOGV2(4920400,
                  "Marked migration state as garbage collectable",
                  "migrationId"_attr = _stateDoc.getId(),
                  "expireAt"_attr = _stateDoc.getExpireAt());
            return status;
        })
        .semi();
}

}  // namespace mongo
