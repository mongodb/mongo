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
#include "mongo/config.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/util/cancelation.h"
#include "mongo/util/future_util.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(abortTenantMigrationBeforeLeavingBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationAfterPersitingInitialDonorStateDoc);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingDataSyncState);

const std::string kTTLIndexName = "TenantMigrationDonorTTLIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);

const Seconds kRecipientSyncDataTimeout(30);
const int kMaxRecipientKeyDocsFindAttempts = 10;

bool shouldStopCreatingTTLIndex(Status status, const CancelationToken& token) {
    return status.isOK() || token.isCanceled();
}

bool shouldStopInsertingDonorStateDoc(Status status, const CancelationToken& token) {
    return status.isOK() || status == ErrorCodes::ConflictingOperationInProgress ||
        token.isCanceled();
}

bool shouldStopUpdatingDonorStateDoc(Status status, const CancelationToken& token) {
    return status.isOK() || token.isCanceled();
}

bool shouldStopSendingRecipientCommand(Status status, const CancelationToken& token) {
    return status.isOK() || !ErrorCodes::isRetriableError(status) || token.isCanceled();
}

void checkIfReceivedDonorAbortMigration(const CancelationToken& parent,
                                        const CancelationToken& instance) {
    // If only the instance token was canceled, then we must have gotten donorAbortMigration.
    uassert(ErrorCodes::TenantMigrationAborted,
            "Migration aborted due to receiving donorAbortMigration.",
            !instance.isCanceled() || parent.isCanceled());
}

}  // namespace

ExecutorFuture<void> TenantMigrationDonorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancelationToken& token) {
    return AsyncTry([this] {
               auto nss = getStateDocumentsNS();

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);

               BSONObj result;
               client.runCommand(
                   nss.db().toString(),
                   BSON("createIndexes"
                        << nss.coll().toString() << "indexes"
                        << BSON_ARRAY(BSON("key" << BSON("expireAt" << 1) << "name" << kTTLIndexName
                                                 << "expireAfterSeconds" << 0))),
                   result);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([token](Status status) { return shouldStopCreatingTTLIndex(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

TenantMigrationDonorService::Instance::Instance(ServiceContext* const serviceContext,
                                                const BSONObj& initialState)
    : repl::PrimaryOnlyService::TypedInstance<Instance>(),
      _serviceContext(serviceContext),
      _stateDoc(tenant_migration_access_blocker::parseDonorStateDocument(initialState)),
      _instanceName(kServiceName + "-" + _stateDoc.getTenantId()),
      _recipientUri(
          uassertStatusOK(MongoURI::parse(_stateDoc.getRecipientConnectionString().toString()))) {
    ThreadPool::Options threadPoolOptions(_getRecipientCmdThreadPoolLimits());
    threadPoolOptions.threadNamePrefix = _instanceName + "-";
    threadPoolOptions.poolName = _instanceName + "ThreadPool";
    threadPoolOptions.onCreateThread = [this](const std::string& threadName) {
        Client::initThread(threadName.c_str());
        auto client = Client::getCurrent();
        AuthorizationSession::get(*client)->grantInternalAuthorization(&cc());

        // Ideally, we should also associate the client created by _recipientCmdExecutor with the
        // TenantMigrationDonorService to make the opCtxs created by the task executor get
        // registered in the TenantMigrationDonorService, and killed on stepdown. But that would
        // require passing the pointer to the TenantMigrationService into the Instance and making
        // constructInstance not const so we can set the client's decoration here. Right now there
        // is no need for that since the task executor is only used with scheduleRemoteCommand and
        // no opCtx will be created (the cancelation token is responsible for canceling the
        // outstanding work on the task executor).
        stdx::lock_guard<Client> lk(*client);
        client->setSystemOperationKillableByStepdown(lk);
    };

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();

    auto connPoolOptions = executor::ConnectionPool::Options();
#ifdef MONGO_CONFIG_SSL
    auto donorCertificate = _stateDoc.getDonorCertificateForRecipient();
    auto donorSSLClusterPEMPayload = donorCertificate.getCertificate().toString() + "\n" +
        donorCertificate.getPrivateKey().toString();
    connPoolOptions.transientSSLParams =
        TransientSSLParams{_recipientUri.connectionString(), std::move(donorSSLClusterPEMPayload)};
#endif

    _recipientCmdExecutor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface(
            _instanceName + "-Network", nullptr, std::move(hookList), connPoolOptions));
    _recipientCmdExecutor->startup();

    if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kUninitialized) {
        // The migration was resumed on stepup.
        stdx::lock_guard<Latch> lg(_mutex);

        _durableState.state = _stateDoc.getState();
        if (_stateDoc.getAbortReason()) {
            _durableState.abortReason =
                getStatusFromCommandResult(_stateDoc.getAbortReason().get());
        }

        _initialDonorStateDurablePromise.emplaceValue();

        if (_stateDoc.getState() == TenantMigrationDonorStateEnum::kAborted ||
            _stateDoc.getState() == TenantMigrationDonorStateEnum::kCommitted) {
            _decisionPromise.emplaceValue();
        }
    }
}

TenantMigrationDonorService::Instance::~Instance() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_initialDonorStateDurablePromise.getFuture().isReady());
    invariant(_receiveDonorForgetMigrationPromise.getFuture().isReady());

    // Unlike the TenantMigrationDonorService's scoped task executor which is shut down on stepdown
    // and joined on stepup, _recipientCmdExecutor is only shut down and joined when the Instance
    // is destroyed. This is safe since ThreadPoolTaskExecutor::shutdown() only cancels the
    // outstanding work on the task executor which the cancelation token will already do, and the
    // Instance will be destroyed on stepup so this is equivalent to joining the task executor on
    // stepup.
    _recipientCmdExecutor->shutdown();
    _recipientCmdExecutor->join();
}

boost::optional<BSONObj> TenantMigrationDonorService::Instance::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    // Ignore connMode and sessionMode because tenant migrations are not associated with
    // sessions and they run in a background thread pool.
    BSONObjBuilder bob;
    bob.append("desc", "tenant donor migration");
    bob.append("migrationCompleted", _completionPromise.getFuture().isReady());
    bob.append("receivedCancelation", _instanceCancelationSource.token().isCanceled());
    bob.append("instanceID", _stateDoc.getId().toBSON());
    bob.append("tenantId", _stateDoc.getTenantId());
    bob.append("recipientConnectionString", _stateDoc.getRecipientConnectionString());
    bob.append("readPreference", _stateDoc.getReadPreference().toInnerBSON());
    bob.append("lastDurableState", _durableState.state);
    if (_stateDoc.getExpireAt()) {
        bob.append("expireAt", _stateDoc.getExpireAt()->toString());
    }
    if (_stateDoc.getBlockTimestamp()) {
        bob.append("blockTimestamp", _stateDoc.getBlockTimestamp()->toBSON());
    }
    if (_stateDoc.getCommitOrAbortOpTime()) {
        bob.append("commitOrAbortOpTime", _stateDoc.getCommitOrAbortOpTime()->toBSON());
    }
    if (_stateDoc.getAbortReason()) {
        bob.append("abortReason", _stateDoc.getAbortReason()->toString());
    }
    return bob.obj();
}

Status TenantMigrationDonorService::Instance::checkIfOptionsConflict(
    const TenantMigrationDonorDocument& stateDoc) {
    if (stateDoc.getId() != _stateDoc.getId() ||
        stateDoc.getTenantId() != _stateDoc.getTenantId() ||
        stateDoc.getRecipientConnectionString() != _stateDoc.getRecipientConnectionString() ||
        SimpleBSONObjComparator::kInstance.compare(stateDoc.getReadPreference().toInnerBSON(),
                                                   _stateDoc.getReadPreference().toInnerBSON()) !=
            0) {
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      str::stream()
                          << "Found active migration for tenantId \"" << stateDoc.getTenantId()
                          << "\" with different options " << _stateDoc.toBSON());
    }

    return Status::OK();
}

TenantMigrationDonorService::Instance::DurableState
TenantMigrationDonorService::Instance::getDurableState(OperationContext* opCtx) {
    // Wait for the insert of the state doc to become majority-committed.
    _initialDonorStateDurablePromise.getFuture().get(opCtx);

    stdx::lock_guard<Latch> lg(_mutex);
    return _durableState;
}

void TenantMigrationDonorService::Instance::onReceiveDonorAbortMigration() {
    _instanceCancelationSource.cancel();

    stdx::lock_guard<Latch> lg(_mutex);
    if (auto fetcher = _recipientKeysFetcher.lock()) {
        fetcher->shutdown();
    }
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
    if (!_initialDonorStateDurablePromise.getFuture().isReady()) {
        _initialDonorStateDurablePromise.setError(status);
    }
    if (!_receiveDonorForgetMigrationPromise.getFuture().isReady()) {
        _receiveDonorForgetMigrationPromise.setError(status);
    }
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
    if (!_decisionPromise.getFuture().isReady()) {
        _decisionPromise.setError(status);
    }
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_fetchAndStoreRecipientClusterTimeKeyDocs(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancelationToken& token) {
    return recipientTargeterRS
        ->findHost(kPrimaryOnlyReadPreference, _instanceCancelationSource.token())
        .thenRunOn(**executor)
        .then([this, self = shared_from_this(), executor](HostAndPort host) {
            const auto nss = NamespaceString::kKeysCollectionNamespace;

            const auto cmdObj = [&] {
                FindCommand request(NamespaceStringOrUUID{nss});
                request.setReadConcern(
                    repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern)
                        .toBSONInner());
                return request.toBSON(BSONObj());
            }();

            std::vector<ExternalKeysCollectionDocument> keyDocs;
            boost::optional<Status> fetchStatus;

            auto fetcherCallback = [this, self = shared_from_this(), &keyDocs, &fetchStatus](
                                       const Fetcher::QueryResponseStatus& dataStatus,
                                       Fetcher::NextAction* nextAction,
                                       BSONObjBuilder* getMoreBob) {
                // Throw out any accumulated results on error
                if (!dataStatus.isOK()) {
                    fetchStatus = dataStatus.getStatus();
                    keyDocs.clear();
                    return;
                }

                const auto& data = dataStatus.getValue();
                for (const BSONObj& doc : data.documents) {
                    keyDocs.push_back(tenant_migration_util::makeExternalClusterTimeKeyDoc(
                        _serviceContext, _recipientUri.getSetName(), doc.getOwned()));
                }
                fetchStatus = Status::OK();

                if (!getMoreBob) {
                    return;
                }
                getMoreBob->append("getMore", data.cursorId);
                getMoreBob->append("collection", data.nss.coll());
            };

            auto fetcher = std::make_shared<Fetcher>(
                _recipientCmdExecutor.get(),
                host,
                nss.db().toString(),
                cmdObj,
                fetcherCallback,
                kPrimaryOnlyReadPreference.toContainingBSON(),
                executor::RemoteCommandRequest::kNoTimeout, /* findNetworkTimeout */
                executor::RemoteCommandRequest::kNoTimeout, /* getMoreNetworkTimeout */
                RemoteCommandRetryScheduler::makeRetryPolicy<ErrorCategory::RetriableError>(
                    kMaxRecipientKeyDocsFindAttempts, executor::RemoteCommandRequest::kNoTimeout),
                transport::kEnableSSL);
            uassertStatusOK(fetcher->schedule());

            {
                stdx::lock_guard<Latch> lg(_mutex);
                _recipientKeysFetcher = fetcher;
            }

            fetcher->join();

            {
                stdx::lock_guard<Latch> lg(_mutex);
                _recipientKeysFetcher.reset();
            }

            if (!fetchStatus) {
                // The callback never got invoked.
                uasserted(5340400, "Internal error running cursor callback in command");
            }
            uassertStatusOK(fetchStatus.get());

            return keyDocs;
        })
        .then([this, self = shared_from_this(), executor, token](auto keyDocs) {
            checkIfReceivedDonorAbortMigration(token, _instanceCancelationSource.token());

            return tenant_migration_util::storeExternalClusterTimeKeyDocsAndRefreshCache(
                executor, std::move(keyDocs), _instanceCancelationSource.token());
        });
}

ExecutorFuture<repl::OpTime> TenantMigrationDonorService::Instance::_insertStateDoc(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    invariant(_stateDoc.getState() == TenantMigrationDonorStateEnum::kUninitialized);
    _stateDoc.setState(TenantMigrationDonorStateEnum::kDataSync);

    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx, "TenantMigrationDonorInsertStateDoc", _stateDocumentsNS.ns(), [&] {
                       const auto filter =
                           BSON(TenantMigrationDonorDocument::kIdFieldName << _stateDoc.getId());
                       const auto updateMod = BSON("$setOnInsert" << _stateDoc.toBSON());
                       auto updateResult = Helpers::upsert(
                           opCtx, _stateDocumentsNS.ns(), filter, updateMod, /*fromMigrate=*/false);

                       // '$setOnInsert' update operator can never modify an existing on-disk state
                       // doc.
                       invariant(!updateResult.numDocsModified);
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([this, self = shared_from_this()](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopInsertingDonorStateDoc(swOpTime.getStatus(),
                                                    _instanceCancelationSource.token());
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

ExecutorFuture<repl::OpTime> TenantMigrationDonorService::Instance::_updateStateDoc(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const TenantMigrationDonorStateEnum nextState) {
    const auto originalStateDocBson = _stateDoc.toBSON();

    return AsyncTry([this, self = shared_from_this(), executor, nextState, originalStateDocBson] {
               boost::optional<repl::OpTime> updateOpTime;

               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               uassert(ErrorCodes::NamespaceNotFound,
                       str::stream() << _stateDocumentsNS.ns() << " does not exist",
                       collection);

               writeConflictRetry(
                   opCtx, "TenantMigrationDonorUpdateStateDoc", _stateDocumentsNS.ns(), [&] {
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
                           case TenantMigrationDonorStateEnum::kBlocking: {
                               _stateDoc.setBlockTimestamp(oplogSlot.getTimestamp());

                               auto mtab = tenant_migration_access_blocker::
                                   getTenantMigrationDonorAccessBlocker(_serviceContext,
                                                                        _stateDoc.getTenantId());
                               invariant(mtab);

                               mtab->startBlockingWrites();
                               opCtx->recoveryUnit()->onRollback(
                                   [mtab] { mtab->rollBackStartBlocking(); });
                               break;
                           }
                           case TenantMigrationDonorStateEnum::kCommitted:
                               _stateDoc.setCommitOrAbortOpTime(oplogSlot);
                               break;
                           case TenantMigrationDonorStateEnum::kAborted: {
                               _stateDoc.setCommitOrAbortOpTime(oplogSlot);

                               invariant(_abortReason);
                               BSONObjBuilder bob;
                               _abortReason.get().serializeErrorToBSON(&bob);
                               _stateDoc.setAbortReason(bob.obj());
                               break;
                           }
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
                   });

               invariant(updateOpTime);
               return updateOpTime.get();
           })
        .until([this, self = shared_from_this()](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopUpdatingDonorStateDoc(swOpTime.getStatus(),
                                                   _instanceCancelationSource.token());
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

ExecutorFuture<repl::OpTime>
TenantMigrationDonorService::Instance::_markStateDocAsGarbageCollectable(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    _stateDoc.setExpireAt(_serviceContext->getFastClockSource()->now() +
                          Milliseconds{repl::tenantMigrationGarbageCollectionDelayMS.load()});
    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx,
                   "TenantMigrationDonorMarkStateDocAsGarbageCollectable",
                   _stateDocumentsNS.ns(),
                   [&] {
                       const auto filter =
                           BSON(TenantMigrationDonorDocument::kIdFieldName << _stateDoc.getId());
                       const auto updateMod = _stateDoc.toBSON();
                       auto updateResult = Helpers::upsert(
                           opCtx, _stateDocumentsNS.ns(), filter, updateMod, /*fromMigrate=*/false);

                       invariant(updateResult.numDocsModified == 1);
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([this, self = shared_from_this()](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopUpdatingDonorStateDoc(swOpTime.getStatus(),
                                                   _instanceCancelationSource.token());
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_waitForMajorityWriteConcern(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, repl::OpTime opTime) {
    return WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(std::move(opTime))
        .thenRunOn(**executor)
        .then([this, self = shared_from_this()] {
            stdx::lock_guard<Latch> lg(_mutex);
            _durableState.state = _stateDoc.getState();
            switch (_durableState.state) {
                case TenantMigrationDonorStateEnum::kDataSync:
                    if (!_initialDonorStateDurablePromise.getFuture().isReady()) {
                        _initialDonorStateDurablePromise.emplaceValue();
                    }
                    break;
                case TenantMigrationDonorStateEnum::kBlocking:
                case TenantMigrationDonorStateEnum::kCommitted:
                    break;
                case TenantMigrationDonorStateEnum::kAborted:
                    invariant(_abortReason);
                    _durableState.abortReason = _abortReason;
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        });
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendCommandToRecipient(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const BSONObj& cmdObj) {
    return AsyncTry([this, self = shared_from_this(), executor, recipientTargeterRS, cmdObj] {
               return recipientTargeterRS
                   ->findHost(kPrimaryOnlyReadPreference, _instanceCancelationSource.token())
                   .thenRunOn(**executor)
                   .then([this, self = shared_from_this(), executor, cmdObj](auto recipientHost) {
                       executor::RemoteCommandRequest request(std::move(recipientHost),
                                                              NamespaceString::kAdminDb.toString(),
                                                              std::move(cmdObj),
                                                              rpc::makeEmptyMetadata(),
                                                              nullptr,
                                                              kRecipientSyncDataTimeout);
                       request.sslMode = transport::kEnableSSL;

                       return (_recipientCmdExecutor)
                           ->scheduleRemoteCommand(std::move(request),
                                                   _instanceCancelationSource.token())
                           .then([this, self = shared_from_this()](const auto& response) -> Status {
                               if (!response.isOK()) {
                                   return response.status;
                               }
                               auto commandStatus = getStatusFromCommandResult(response.data);
                               commandStatus.addContext(
                                   "Tenant migration recipient command failed");
                               return commandStatus;
                           });
                   });
           })
        .until([this, self = shared_from_this()](Status status) {
            return shouldStopSendingRecipientCommand(status, _instanceCancelationSource.token());
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, _instanceCancelationSource.token());
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendRecipientSyncDataCommand(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS) {

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

    const auto cmdObj = [&] {
        auto donorConnString =
            repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString();
        RecipientSyncData request;
        request.setDbName(NamespaceString::kAdminDb);
        request.setMigrationRecipientCommonData({_stateDoc.getId(),
                                                 donorConnString.toString(),
                                                 _stateDoc.getTenantId().toString(),
                                                 _stateDoc.getReadPreference(),
                                                 _stateDoc.getRecipientCertificateForDonor()});
        request.setReturnAfterReachingDonorTimestamp(_stateDoc.getBlockTimestamp());
        return request.toBSON(BSONObj());
    }();

    return _sendCommandToRecipient(executor, recipientTargeterRS, cmdObj);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendRecipientForgetMigrationCommand(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS) {

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

    auto donorConnString =
        repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString();
    RecipientForgetMigration request;
    request.setDbName(NamespaceString::kAdminDb);
    request.setMigrationRecipientCommonData({_stateDoc.getId(),
                                             donorConnString.toString(),
                                             _stateDoc.getTenantId().toString(),
                                             _stateDoc.getReadPreference(),
                                             _stateDoc.getRecipientCertificateForDonor()});
    return _sendCommandToRecipient(executor, recipientTargeterRS, request.toBSON(BSONObj()));
}

SemiFuture<void> TenantMigrationDonorService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancelationToken& token) noexcept {
    _instanceCancelationSource = CancelationSource(token);
    auto recipientTargeterRS = std::make_shared<RemoteCommandTargeterRS>(
        _recipientUri.getSetName(), _recipientUri.getServers());

    return ExecutorFuture<void>(**executor)
        .then([this, self = shared_from_this(), executor, token] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kUninitialized) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            // Enter "dataSync" state.
            return _insertStateDoc(executor)
                .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
                    // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
                    // use its base PrimaryOnlyService's cancelation source to pass tokens
                    // in calls to WaitForMajorityService::waitUntilMajority.
                    return _waitForMajorityWriteConcern(executor, std::move(opTime));
                })
                .then([this, self = shared_from_this()] {
                    auto opCtxHolder = cc().makeOperationContext();
                    auto opCtx = opCtxHolder.get();
                    pauseTenantMigrationAfterPersitingInitialDonorStateDoc.pauseWhileSet(opCtx);
                });
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, token] {
            checkIfReceivedDonorAbortMigration(token, _instanceCancelationSource.token());

            return _fetchAndStoreRecipientClusterTimeKeyDocs(executor, recipientTargeterRS, token);
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, token] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kDataSync) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            checkIfReceivedDonorAbortMigration(token, _instanceCancelationSource.token());

            return _sendRecipientSyncDataCommand(executor, recipientTargeterRS)
                .then([this, self = shared_from_this()] {
                    auto opCtxHolder = cc().makeOperationContext();
                    auto opCtx = opCtxHolder.get();
                    pauseTenantMigrationBeforeLeavingDataSyncState.pauseWhileSet(opCtx);
                })
                .then([this, self = shared_from_this(), executor, token] {
                    checkIfReceivedDonorAbortMigration(token, _instanceCancelationSource.token());

                    // Enter "blocking" state.
                    return _updateStateDoc(executor, TenantMigrationDonorStateEnum::kBlocking)
                        .then([this, self = shared_from_this(), executor, token](
                                  repl::OpTime opTime) {
                            // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
                            // use its base PrimaryOnlyService's cancelation source to pass tokens
                            // in calls to WaitForMajorityService::waitUntilMajority.
                            checkIfReceivedDonorAbortMigration(token,
                                                               _instanceCancelationSource.token());

                            return _waitForMajorityWriteConcern(executor, std::move(opTime));
                        });
                });
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, token] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kBlocking) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            checkIfReceivedDonorAbortMigration(token, _instanceCancelationSource.token());

            invariant(_stateDoc.getBlockTimestamp());
            // Source to cancel the timeout if the operation completed in time.
            CancelationSource cancelTimeoutSource;
            // Source to cancel if the timeout expires before completion, as a child of parent
            // token.
            CancelationSource recipientSyncDataCommandCancelSource(token);

            auto deadlineReachedFuture = (*executor)->sleepFor(
                Milliseconds(repl::tenantMigrationBlockingStateTimeoutMS.load()),
                cancelTimeoutSource.token());
            std::vector<ExecutorFuture<void>> futures;

            futures.push_back(std::move(deadlineReachedFuture));
            futures.push_back(_sendRecipientSyncDataCommand(executor, recipientTargeterRS));

            return whenAny(std::move(futures))
                .thenRunOn(**executor)
                .then([cancelTimeoutSource,
                       recipientSyncDataCommandCancelSource,
                       self = shared_from_this()](auto result) mutable {
                    const auto& [status, idx] = result;

                    if (idx == 0) {
                        LOGV2(5290301,
                              "Tenant migration blocking stage timeout expired",
                              "timeoutMs"_attr =
                                  repl::tenantMigrationGarbageCollectionDelayMS.load());
                        // Deadline reached, cancel the pending '_sendRecipientSyncDataCommand()'...
                        recipientSyncDataCommandCancelSource.cancel();
                        // ...and return error.
                        uasserted(ErrorCodes::ExceededTimeLimit, "Blocking state timeout expired");
                    } else if (idx == 1) {
                        // '_sendRecipientSyncDataCommand()' finished first, cancel the timeout.
                        cancelTimeoutSource.cancel();
                        return status;
                    }
                    MONGO_UNREACHABLE;
                })
                .then([this, self = shared_from_this()]() -> void {
                    auto opCtxHolder = cc().makeOperationContext();
                    auto opCtx = opCtxHolder.get();

                    pauseTenantMigrationBeforeLeavingBlockingState.executeIf(
                        [&](const BSONObj&) {
                            pauseTenantMigrationBeforeLeavingBlockingState.pauseWhileSet(opCtx);
                        },
                        [&](const BSONObj& data) {
                            return !data.hasField("tenantId") ||
                                _stateDoc.getTenantId() == data["tenantId"].str();
                        });

                    abortTenantMigrationBeforeLeavingBlockingState.execute(
                        [&](const BSONObj& data) {
                            if (data.hasField("blockTimeMS")) {
                                const auto blockTime =
                                    Milliseconds{data.getIntField("blockTimeMS")};
                                LOGV2(5010400,
                                      "Keep migration in blocking state before aborting",
                                      "blockTime"_attr = blockTime);
                                opCtx->sleepFor(blockTime);
                            }

                            uasserted(ErrorCodes::InternalError,
                                      "simulate a tenant migration error");
                        });
                })
                .then([this, self = shared_from_this(), executor, token] {
                    checkIfReceivedDonorAbortMigration(token, _instanceCancelationSource.token());

                    // Enter "commit" state.
                    return _updateStateDoc(executor, TenantMigrationDonorStateEnum::kCommitted)
                        .then([this, self = shared_from_this(), executor, token](
                                  repl::OpTime opTime) {
                            // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
                            // use its base PrimaryOnlyService's cancelation source to pass tokens
                            // in calls to WaitForMajorityService::waitUntilMajority.
                            return _waitForMajorityWriteConcern(executor, std::move(opTime))
                                .then([this, self = shared_from_this()] {
                                    // If interrupt is called at some point during execution, it is
                                    // possible that interrupt() will fulfill the promise before we
                                    // do.
                                    if (!_decisionPromise.getFuture().isReady()) {
                                        // Fulfill the promise since we have made a decision.
                                        _decisionPromise.emplaceValue();
                                    }
                                });
                        });
                });
        })
        .onError([this, self = shared_from_this(), executor](Status status) {
            if (_stateDoc.getState() == TenantMigrationDonorStateEnum::kAborted) {
                // The migration was resumed on stepup and it was already aborted.
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(
                _serviceContext, _stateDoc.getTenantId());
            if (status == ErrorCodes::ConflictingOperationInProgress || !mtab) {
                stdx::lock_guard<Latch> lg(_mutex);
                if (!_initialDonorStateDurablePromise.getFuture().isReady()) {
                    // Fulfill the promise since the state doc failed to insert.
                    _initialDonorStateDurablePromise.setError(status);
                }

                return ExecutorFuture<void>(**executor, status);
            } else {
                // Enter "abort" state.
                _abortReason.emplace(status);
                return _updateStateDoc(executor, TenantMigrationDonorStateEnum::kAborted)
                    .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
                        return _waitForMajorityWriteConcern(executor, std::move(opTime))
                            .then([this, self = shared_from_this()] {
                                // If interrupt is called at some point during execution, it is
                                // possible that interrupt() will fulfill the promise before we do.
                                if (!_decisionPromise.getFuture().isReady()) {
                                    // Fulfill the promise since we have made a decision.
                                    _decisionPromise.emplaceValue();
                                };
                            });
                    });
            }
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            LOGV2(5006601,
                  "Tenant migration completed",
                  "migrationId"_attr = _stateDoc.getId(),
                  "tenantId"_attr = _stateDoc.getTenantId(),
                  "status"_attr = status,
                  "abortReason"_attr = _abortReason);
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS] {
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
                .then([this, self = shared_from_this(), executor, recipientTargeterRS] {
                    return _sendRecipientForgetMigrationCommand(executor, recipientTargeterRS);
                })
                .then([this, self = shared_from_this(), executor] {
                    return _markStateDocAsGarbageCollectable(executor);
                })
                .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
                    return _waitForMajorityWriteConcern(executor, std::move(opTime));
                });
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            LOGV2(4920400,
                  "Marked migration state as garbage collectable",
                  "migrationId"_attr = _stateDoc.getId(),
                  "expireAt"_attr = _stateDoc.getExpireAt());

            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here
                return;
            }

            if (status.isOK()) {
                _completionPromise.emplaceValue();
            } else {
                _completionPromise.setError(status);
            }
        })
        .semi();
}

}  // namespace mongo
