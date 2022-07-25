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


#include "mongo/db/repl/tenant_migration_donor_service.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_migration_statistics.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(abortTenantMigrationBeforeLeavingBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationAfterPersistingInitialDonorStateDoc);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingDataSyncState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeFetchingKeys);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationDonorBeforeWaitingForKeysToReplicate);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationDonorBeforeMarkingStateGarbageCollectable);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationDonorAfterMarkingStateGarbageCollectable);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeEnteringFutureChain);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationAfterFetchingAndStoringKeys);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationDonorWhileUpdatingStateDoc);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeInsertingDonorStateDoc);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeCreatingStateDocumentTTLIndex);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeCreatingExternalKeysTTLIndex);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingCommittedState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationAfterUpdatingToCommittedState);

const std::string kTTLIndexName = "TenantMigrationDonorTTLIndex";
const std::string kExternalKeysTTLIndexName = "ExternalKeysTTLIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);

const int kMaxRecipientKeyDocsFindAttempts = 10;

bool shouldStopSendingRecipientForgetMigrationCommand(Status status) {
    return status.isOK() ||
        !(ErrorCodes::isRetriableError(status) || ErrorCodes::isNetworkTimeoutError(status) ||
          // Returned if findHost() is unable to target the recipient in 15 seconds, which may
          // happen after a failover.
          status == ErrorCodes::FailedToSatisfyReadPreference ||
          ErrorCodes::isInterruption(status));
}

bool shouldStopSendingRecipientSyncDataCommand(Status status, MigrationProtocolEnum protocol) {
    if (status.isOK() || protocol == MigrationProtocolEnum::kShardMerge) {
        return true;
    }

    return !(ErrorCodes::isRetriableError(status) || ErrorCodes::isNetworkTimeoutError(status) ||
             // Returned if findHost() is unable to target the recipient in 15 seconds, which may
             // happen after a failover.
             status == ErrorCodes::FailedToSatisfyReadPreference);
}

bool shouldStopFetchingRecipientClusterTimeKeyDocs(Status status) {
    return status.isOK() ||
        !(ErrorCodes::isRetriableError(status) || ErrorCodes::isInterruption(status) ||
          ErrorCodes::isNetworkTimeoutError(status) ||
          // Returned if findHost() is unable to target the recipient in 15 seconds, which may
          // happen after a failover.
          status == ErrorCodes::FailedToSatisfyReadPreference);
}

void checkForTokenInterrupt(const CancellationToken& token) {
    uassert(ErrorCodes::CallbackCanceled, "Donor service interrupted", !token.isCanceled());
}


template <class Promise>
void setPromiseFromStatusIfNotReady(WithLock lk, Promise& promise, Status status) {
    if (promise.getFuture().isReady()) {
        return;
    }

    if (status.isOK()) {
        promise.emplaceValue();
    } else {
        promise.setError(status);
    }
}

template <class Promise>
void setPromiseErrorIfNotReady(WithLock lk, Promise& promise, Status status) {
    if (promise.getFuture().isReady()) {
        return;
    }

    promise.setError(status);
}

template <class Promise>
void setPromiseOkIfNotReady(WithLock lk, Promise& promise) {
    if (promise.getFuture().isReady()) {
        return;
    }

    promise.emplaceValue();
}

}  // namespace

void TenantMigrationDonorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) {
    auto stateDoc = tenant_migration_access_blocker::parseDonorStateDocument(initialState);
    auto isNewShardMerge = stateDoc.getProtocol() == MigrationProtocolEnum::kShardMerge;

    for (auto& instance : existingInstances) {
        auto existingTypedInstance =
            checked_cast<const TenantMigrationDonorService::Instance*>(instance);
        auto existingState = existingTypedInstance->getDurableState();
        auto existingIsAborted = existingState &&
            existingState->state == TenantMigrationDonorStateEnum::kAborted &&
            existingState->expireAt;

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Cannot start a shard merge with existing migrations in progress",
                !isNewShardMerge || existingIsAborted);

        uassert(
            ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot start a migration with an existing shard merge in progress",
            existingTypedInstance->getProtocol() != MigrationProtocolEnum::kShardMerge ||
                existingIsAborted);

        // Any existing migration for this tenant must be aborted and garbage-collectable.
        if (existingTypedInstance->getTenantId() == stateDoc.getTenantId()) {
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "tenant " << stateDoc.getTenantId() << " is already migrating",
                    existingIsAborted);
        }
    }
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> TenantMigrationDonorService::constructInstance(
    BSONObj initialState) {

    return std::make_shared<TenantMigrationDonorService::Instance>(
        _serviceContext, this, initialState);
}  // namespace mongo

void TenantMigrationDonorService::abortAllMigrations(OperationContext* opCtx) {
    LOGV2(5356301, "Aborting all tenant migrations on donor");
    auto instances = getAllInstances(opCtx);
    for (auto& instance : instances) {
        auto typedInstance = checked_pointer_cast<TenantMigrationDonorService::Instance>(instance);
        typedInstance->onReceiveDonorAbortMigration();
    }
}

// Note this index is required on both the donor and recipient in a tenant migration, since each
// will copy cluster time keys from the other. The donor service is set up on all mongods on stepup
// to primary, so this index will be created on both donors and recipients.
ExecutorFuture<void> TenantMigrationDonorService::createStateDocumentTTLIndex(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return AsyncTry([this] {
               auto nss = getStateDocumentsNS();

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);

               pauseTenantMigrationBeforeCreatingStateDocumentTTLIndex.pauseWhileSet(opCtx);

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
        .until([](Status status) { return status.isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> TenantMigrationDonorService::createExternalKeysTTLIndex(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return AsyncTry([this] {
               const auto nss = NamespaceString::kExternalKeysCollectionNamespace;

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);

               pauseTenantMigrationBeforeCreatingExternalKeysTTLIndex.pauseWhileSet(opCtx);

               BSONObj result;
               client.runCommand(
                   nss.db().toString(),
                   BSON("createIndexes"
                        << nss.coll().toString() << "indexes"
                        << BSON_ARRAY(BSON("key" << BSON("ttlExpiresAt" << 1) << "name"
                                                 << kExternalKeysTTLIndexName
                                                 << "expireAfterSeconds" << 0))),
                   result);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([](Status status) { return status.isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> TenantMigrationDonorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return createStateDocumentTTLIndex(executor, token).then([this, executor, token] {
        return createExternalKeysTTLIndex(executor, token);
    });
}

TenantMigrationDonorService::Instance::Instance(ServiceContext* const serviceContext,
                                                const TenantMigrationDonorService* donorService,
                                                const BSONObj& initialState)
    : repl::PrimaryOnlyService::TypedInstance<Instance>(),
      _serviceContext(serviceContext),
      _donorService(donorService),
      _stateDoc(tenant_migration_access_blocker::parseDonorStateDocument(initialState)),
      _instanceName(kServiceName + "-" + _stateDoc.getId().toString()),
      _recipientUri(
          uassertStatusOK(MongoURI::parse(_stateDoc.getRecipientConnectionString().toString()))),
      _tenantId(_stateDoc.getTenantId()),
      _protocol(_stateDoc.getProtocol().value_or(MigrationProtocolEnum::kMultitenantMigrations)),
      _recipientConnectionString(_stateDoc.getRecipientConnectionString()),
      _readPreference(_stateDoc.getReadPreference()),
      _migrationUuid(_stateDoc.getId()),
      _donorCertificateForRecipient(_stateDoc.getDonorCertificateForRecipient()),
      _recipientCertificateForDonor(_stateDoc.getRecipientCertificateForDonor()),
      _sslMode(repl::tenantMigrationDisableX509Auth ? transport::kGlobalSSLMode
                                                    : transport::kEnableSSL) {

    _recipientCmdExecutor = _makeRecipientCmdExecutor();
    _recipientCmdExecutor->startup();

    if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kUninitialized) {
        // The migration was resumed on stepup.

        if (_stateDoc.getAbortReason()) {
            auto abortReasonBson = _stateDoc.getAbortReason().get();
            auto code = abortReasonBson["code"].Int();
            auto errmsg = abortReasonBson["errmsg"].String();
            _abortReason = Status(ErrorCodes::Error(code), errmsg);
        }
        _durableState = DurableState{_stateDoc.getState(), _abortReason, _stateDoc.getExpireAt()};

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
    // outstanding work on the task executor which the cancellation token will already do, and the
    // Instance will be destroyed on stepup so this is equivalent to joining the task executor on
    // stepup.
    _recipientCmdExecutor->shutdown();
    _recipientCmdExecutor->join();
}

std::shared_ptr<executor::ThreadPoolTaskExecutor>
TenantMigrationDonorService::Instance::_makeRecipientCmdExecutor() {
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
        // no opCtx will be created (the cancellation token is responsible for canceling the
        // outstanding work on the task executor).
        stdx::lock_guard<Client> lk(*client);
        client->setSystemOperationKillableByStepdown(lk);
    };

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();

    auto connPoolOptions = executor::ConnectionPool::Options();
    if (_donorCertificateForRecipient) {
        invariant(!repl::tenantMigrationDisableX509Auth);
        invariant(_recipientCertificateForDonor);
        invariant(_sslMode == transport::kEnableSSL);
#ifdef MONGO_CONFIG_SSL
        uassert(ErrorCodes::IllegalOperation,
                "Cannot run tenant migration with x509 authentication as SSL is not enabled",
                getSSLGlobalParams().sslMode.load() != SSLParams::SSLMode_disabled);
        auto donorSSLClusterPEMPayload =
            _donorCertificateForRecipient->getCertificate().toString() + "\n" +
            _donorCertificateForRecipient->getPrivateKey().toString();
        connPoolOptions.transientSSLParams = TransientSSLParams{
            _recipientUri.connectionString(), std::move(donorSSLClusterPEMPayload)};
#else
        // If SSL is not supported, the donorStartMigration command should have failed certificate
        // field validation.
        MONGO_UNREACHABLE;
#endif
    } else {
        invariant(repl::tenantMigrationDisableX509Auth);
        invariant(!_recipientCertificateForDonor);
        invariant(_sslMode == transport::kGlobalSSLMode);
    }

    return std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface(
            _instanceName + "-Network", nullptr, std::move(hookList), connPoolOptions));
}

boost::optional<BSONObj> TenantMigrationDonorService::Instance::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    stdx::lock_guard<Latch> lg(_mutex);

    // Ignore connMode and sessionMode because tenant migrations are not associated with
    // sessions and they run in a background thread pool.
    BSONObjBuilder bob;
    bob.append("desc", "tenant donor migration");
    bob.append("migrationCompleted", _completionPromise.getFuture().isReady());
    _migrationUuid.appendToBuilder(&bob, "instanceID"_sd);
    if (getProtocol() == MigrationProtocolEnum::kMultitenantMigrations) {
        bob.append("tenantId", _tenantId);
    }
    bob.append("recipientConnectionString", _recipientConnectionString);
    bob.append("readPreference", _readPreference.toInnerBSON());
    bob.append("receivedCancellation", _abortRequested);
    if (_durableState) {
        bob.append("lastDurableState", _durableState.get().state);
    } else {
        bob.appendUndefined("lastDurableState");
    }
    if (_stateDoc.getMigrationStart()) {
        bob.appendDate("migrationStart", *_stateDoc.getMigrationStart());
    }
    if (_stateDoc.getExpireAt()) {
        bob.appendDate("expireAt", *_stateDoc.getExpireAt());
    }
    if (_stateDoc.getStartMigrationDonorTimestamp()) {
        bob.append("startMigrationDonorTimestamp", *_stateDoc.getStartMigrationDonorTimestamp());
    }
    if (_stateDoc.getBlockTimestamp()) {
        bob.append("blockTimestamp", *_stateDoc.getBlockTimestamp());
    }
    if (_stateDoc.getCommitOrAbortOpTime()) {
        _stateDoc.getCommitOrAbortOpTime()->append(&bob, "commitOrAbortOpTime");
    }
    if (_stateDoc.getAbortReason()) {
        bob.append("abortReason", *_stateDoc.getAbortReason());
    }
    return bob.obj();
}

void TenantMigrationDonorService::Instance::checkIfOptionsConflict(const BSONObj& options) const {
    auto stateDoc = tenant_migration_access_blocker::parseDonorStateDocument(options);

    invariant(stateDoc.getId() == _migrationUuid);
    invariant(stateDoc.getProtocol());

    if (stateDoc.getProtocol().value() != _protocol || stateDoc.getTenantId() != _tenantId ||
        stateDoc.getRecipientConnectionString() != _recipientConnectionString ||
        !stateDoc.getReadPreference().equals(_readPreference) ||
        stateDoc.getDonorCertificateForRecipient() != _donorCertificateForRecipient ||
        stateDoc.getRecipientCertificateForDonor() != _recipientCertificateForDonor) {
        uasserted(ErrorCodes::ConflictingOperationInProgress,
                  str::stream() << "Found active migration for migrationId \""
                                << _migrationUuid.toBSON() << "\" with different options "
                                << tenant_migration_util::redactStateDoc(_stateDoc.toBSON()));
    }
}

boost::optional<TenantMigrationDonorService::Instance::DurableState>
TenantMigrationDonorService::Instance::getDurableState() const {
    stdx::lock_guard<Latch> lg(_mutex);
    return _durableState;
}

void TenantMigrationDonorService::Instance::onReceiveDonorAbortMigration() {
    stdx::lock_guard<Latch> lg(_mutex);
    _abortRequested = true;
    if (_abortMigrationSource) {
        _abortMigrationSource->cancel();
    }
    if (auto fetcher = _recipientKeysFetcher.lock()) {
        fetcher->shutdown();
    }
}

void TenantMigrationDonorService::Instance::onReceiveDonorForgetMigration() {
    stdx::lock_guard<Latch> lg(_mutex);
    setPromiseOkIfNotReady(lg, _receiveDonorForgetMigrationPromise);
}

void TenantMigrationDonorService::Instance::interrupt(Status status) {
    stdx::lock_guard<Latch> lg(_mutex);
    // Resolve any unresolved promises to avoid hanging.
    setPromiseErrorIfNotReady(lg, _initialDonorStateDurablePromise, status);
    setPromiseErrorIfNotReady(lg, _receiveDonorForgetMigrationPromise, status);
    setPromiseErrorIfNotReady(lg, _completionPromise, status);
    setPromiseErrorIfNotReady(lg, _decisionPromise, status);

    if (auto fetcher = _recipientKeysFetcher.lock()) {
        fetcher->shutdown();
    }
}

ExecutorFuture<repl::OpTime> TenantMigrationDonorService::Instance::_insertStateDoc(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_stateDoc.getState() == TenantMigrationDonorStateEnum::kUninitialized);
    _stateDoc.setState(TenantMigrationDonorStateEnum::kAbortingIndexBuilds);

    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               pauseTenantMigrationBeforeInsertingDonorStateDoc.pauseWhileSet(opCtx);

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx, "TenantMigrationDonorInsertStateDoc", _stateDocumentsNS.ns(), [&] {
                       const auto filter =
                           BSON(TenantMigrationDonorDocument::kIdFieldName << _migrationUuid);
                       const auto updateMod = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return BSON("$setOnInsert" << _stateDoc.toBSON());
                       }();
                       auto updateResult = Helpers::upsert(
                           opCtx, _stateDocumentsNS, filter, updateMod, /*fromMigrate=*/false);

                       // '$setOnInsert' update operator can never modify an existing on-disk state
                       // doc.
                       invariant(!updateResult.numDocsModified);
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([](StatusWith<repl::OpTime> swOpTime) { return swOpTime.getStatus().isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<repl::OpTime> TenantMigrationDonorService::Instance::_updateStateDoc(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const TenantMigrationDonorStateEnum nextState,
    const CancellationToken& token) {
    stdx::lock_guard<Latch> lg(_mutex);

    const auto originalStateDocBson = _stateDoc.toBSON();

    return AsyncTry([this, self = shared_from_this(), executor, nextState, originalStateDocBson] {
               boost::optional<repl::OpTime> updateOpTime;

               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               pauseTenantMigrationDonorWhileUpdatingStateDoc.pauseWhileSet(opCtx);

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               uassert(ErrorCodes::NamespaceNotFound,
                       str::stream() << _stateDocumentsNS.ns() << " does not exist",
                       collection);

               writeConflictRetry(
                   opCtx, "TenantMigrationDonorUpdateStateDoc", _stateDocumentsNS.ns(), [&] {
                       WriteUnitOfWork wuow(opCtx);

                       const auto originalRecordId = Helpers::findOne(
                           opCtx, collection.getCollection(), originalStateDocBson);
                       const auto originalSnapshot = Snapshotted<BSONObj>(
                           opCtx->recoveryUnit()->getSnapshotId(), originalStateDocBson);
                       invariant(!originalRecordId.isNull());

                       if (nextState == TenantMigrationDonorStateEnum::kBlocking) {
                           // Start blocking writes before getting an oplog slot to guarantee no
                           // writes to the tenant's data can commit with a timestamp after the
                           // block timestamp.
                           auto mtab = tenant_migration_access_blocker::
                               getTenantMigrationDonorAccessBlocker(_serviceContext, _tenantId);
                           invariant(mtab);
                           mtab->startBlockingWrites();

                           opCtx->recoveryUnit()->onRollback(
                               [mtab] { mtab->rollBackStartBlocking(); });
                       }

                       // Reserve an opTime for the write.
                       auto oplogSlot = LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];
                       {
                           stdx::lock_guard<Latch> lg(_mutex);

                           // Update the state.
                           _stateDoc.setState(nextState);
                           switch (nextState) {
                               case TenantMigrationDonorStateEnum::kDataSync: {
                                   _stateDoc.setStartMigrationDonorTimestamp(
                                       oplogSlot.getTimestamp());
                                   break;
                               }
                               case TenantMigrationDonorStateEnum::kBlocking: {
                                   _stateDoc.setBlockTimestamp(oplogSlot.getTimestamp());
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
                       }

                       const auto updatedStateDocBson = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return _stateDoc.toBSON();
                       }();

                       CollectionUpdateArgs args;
                       args.criteria = BSON("_id" << _migrationUuid);
                       args.oplogSlots = {oplogSlot};
                       args.update = updatedStateDocBson;

                       collection->updateDocument(opCtx,
                                                  originalRecordId,
                                                  originalSnapshot,
                                                  updatedStateDocBson,
                                                  false,
                                                  nullptr /* OpDebug* */,
                                                  &args);

                       wuow.commit();

                       if (nextState == TenantMigrationDonorStateEnum::kCommitted) {
                           pauseTenantMigrationAfterUpdatingToCommittedState.pauseWhileSet();
                       }

                       updateOpTime = oplogSlot;
                   });

               invariant(updateOpTime);
               return updateOpTime.get();
           })
        .until([](StatusWith<repl::OpTime> swOpTime) { return swOpTime.getStatus().isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<repl::OpTime>
TenantMigrationDonorService::Instance::_markStateDocAsGarbageCollectable(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    stdx::lock_guard<Latch> lg(_mutex);

    _stateDoc.setExpireAt(_serviceContext->getFastClockSource()->now() +
                          Milliseconds{repl::tenantMigrationGarbageCollectionDelayMS.load()});
    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               pauseTenantMigrationDonorBeforeMarkingStateGarbageCollectable.pauseWhileSet(opCtx);

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx,
                   "TenantMigrationDonorMarkStateDocAsGarbageCollectable",
                   _stateDocumentsNS.ns(),
                   [&] {
                       const auto filter =
                           BSON(TenantMigrationDonorDocument::kIdFieldName << _migrationUuid);
                       const auto updateMod = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return _stateDoc.toBSON();
                       }();
                       auto updateResult = Helpers::upsert(
                           opCtx, _stateDocumentsNS, filter, updateMod, /*fromMigrate=*/false);

                       invariant(updateResult.numDocsModified == 1);
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([](StatusWith<repl::OpTime> swOpTime) { return swOpTime.getStatus().isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_waitForMajorityWriteConcern(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    repl::OpTime opTime,
    const CancellationToken& token) {
    return WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(std::move(opTime), token)
        .thenRunOn(**executor)
        .then([this, self = shared_from_this()] {
            stdx::lock_guard<Latch> lg(_mutex);
            boost::optional<Status> abortReason;
            switch (_stateDoc.getState()) {
                case TenantMigrationDonorStateEnum::kAbortingIndexBuilds:
                    setPromiseOkIfNotReady(lg, _initialDonorStateDurablePromise);
                    break;
                case TenantMigrationDonorStateEnum::kDataSync:
                case TenantMigrationDonorStateEnum::kBlocking:
                case TenantMigrationDonorStateEnum::kCommitted:
                    break;
                case TenantMigrationDonorStateEnum::kAborted:
                    invariant(_abortReason);
                    abortReason = _abortReason;
                    break;
                default:
                    MONGO_UNREACHABLE;
            }

            _durableState =
                DurableState{_stateDoc.getState(), std::move(abortReason), _stateDoc.getExpireAt()};
        });
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendCommandToRecipient(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const BSONObj& cmdObj,
    const CancellationToken& token) {
    const bool isRecipientSyncDataCmd = cmdObj.hasField(RecipientSyncData::kCommandName);
    return AsyncTry(
               [this, self = shared_from_this(), executor, recipientTargeterRS, cmdObj, token] {
                   return recipientTargeterRS->findHost(kPrimaryOnlyReadPreference, token)
                       .thenRunOn(**executor)
                       .then([this, self = shared_from_this(), executor, cmdObj, token](
                                 auto recipientHost) {
                           executor::RemoteCommandRequest request(
                               std::move(recipientHost),
                               NamespaceString::kAdminDb.toString(),
                               std::move(cmdObj),
                               rpc::makeEmptyMetadata(),
                               nullptr);
                           request.sslMode = _sslMode;

                           return (_recipientCmdExecutor)
                               ->scheduleRemoteCommand(std::move(request), token)
                               .then([this,
                                      self = shared_from_this()](const auto& response) -> Status {
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
        .until([this, self = shared_from_this(), token, cmdObj, isRecipientSyncDataCmd](
                   Status status) {
            if (isRecipientSyncDataCmd) {
                return shouldStopSendingRecipientSyncDataCommand(status, getProtocol());
            } else {
                // If the recipient command is not 'recipientSyncData', it must be
                // 'recipientForgetMigration'.
                invariant(cmdObj.hasField(RecipientForgetMigration::kCommandName));
                return shouldStopSendingRecipientForgetMigrationCommand(status);
            }
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendRecipientSyncDataCommand(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& token) {

    const auto cmdObj = [&] {
        auto donorConnString =
            repl::ReplicationCoordinator::get(_serviceContext)->getConfigConnectionString();

        RecipientSyncData request;
        request.setDbName(NamespaceString::kAdminDb);

        MigrationRecipientCommonData commonData(
            _migrationUuid, donorConnString.toString(), _readPreference);
        commonData.setRecipientCertificateForDonor(_recipientCertificateForDonor);
        // TODO SERVER-63454: Pass tenantId only for 'kMultitenantMigrations' protocol.
        commonData.setTenantId(_tenantId);

        stdx::lock_guard<Latch> lg(_mutex);

        commonData.setProtocol(_protocol);
        request.setMigrationRecipientCommonData(commonData);

        invariant(_stateDoc.getStartMigrationDonorTimestamp());
        request.setStartMigrationDonorTimestamp(*_stateDoc.getStartMigrationDonorTimestamp());
        request.setReturnAfterReachingDonorTimestamp(_stateDoc.getBlockTimestamp());
        return request.toBSON(BSONObj());
    }();

    return _sendCommandToRecipient(executor, recipientTargeterRS, cmdObj, token);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendRecipientForgetMigrationCommand(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& token) {

    auto donorConnString =
        repl::ReplicationCoordinator::get(_serviceContext)->getConfigConnectionString();

    RecipientForgetMigration request;
    request.setDbName(NamespaceString::kAdminDb);

    MigrationRecipientCommonData commonData(
        _migrationUuid, donorConnString.toString(), _readPreference);
    commonData.setRecipientCertificateForDonor(_recipientCertificateForDonor);
    // TODO SERVER-63454: Pass tenantId only for 'kMultitenantMigrations' protocol.
    commonData.setTenantId(_tenantId);

    commonData.setProtocol(_protocol);
    request.setMigrationRecipientCommonData(commonData);

    return _sendCommandToRecipient(executor, recipientTargeterRS, request.toBSON(BSONObj()), token);
}

CancellationToken TenantMigrationDonorService::Instance::_initAbortMigrationSource(
    const CancellationToken& token) {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(!_abortMigrationSource);
    _abortMigrationSource = CancellationSource(token);

    if (_abortRequested) {
        // An abort was requested before the abort source was set up so immediately cancel it.
        _abortMigrationSource->cancel();
    }

    return _abortMigrationSource->token();
}

SemiFuture<void> TenantMigrationDonorService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    pauseTenantMigrationBeforeEnteringFutureChain.pauseWhileSet();

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (!_stateDoc.getMigrationStart()) {
            _stateDoc.setMigrationStart(_serviceContext->getFastClockSource()->now());
        }
    }

    auto isFCVUpgradingOrDowngrading = [&]() -> bool {
        // We must abort the migration if we try to start or resume while upgrading or downgrading.
        // (Generic FCV reference): This FCV check should exist across LTS binary versions.
        if (serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading()) {
            LOGV2(5356302, "Must abort tenant migration as donor is upgrading or downgrading");
            return true;
        }
        return false;
    };


    // Tenant migrations gets aborted on FCV upgrading or downgrading state. But,
    // due to race between between Instance::getOrCreate() and
    // SetFeatureCompatibilityVersionCommand::_cancelTenantMigrations(), we might miss aborting this
    // tenant migration and FCV might have updated or downgraded at this point. So, need to ensure
    // that the protocol is still compatible with FCV.
    if (isFCVUpgradingOrDowngrading()) {
        onReceiveDonorAbortMigration();
    }

    // Any FCV changes after this point will abort this migration.
    auto abortToken = _initAbortMigrationSource(token);

    auto recipientTargeterRS = std::make_shared<RemoteCommandTargeterRS>(
        _recipientUri.getSetName(), _recipientUri.getServers());
    auto scopedOutstandingMigrationCounter =
        TenantMigrationStatistics::get(_serviceContext)->getScopedOutstandingDonatingCount();

    return ExecutorFuture(**executor)
        .then([this, self = shared_from_this(), executor, token] {
            LOGV2(6104900,
                  "Entering 'aborting index builds' state.",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            // Note we do not use the abort migration token here because the donorAbortMigration
            // command waits for a decision to be persisted which will not happen if inserting the
            // initial state document fails.
            return _enterAbortingIndexBuildsState(executor, token);
        })
        .then([this, self = shared_from_this(), executor, abortToken] {
            LOGV2(6104901,
                  "Aborting index builds.",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            _abortIndexBuilds(abortToken);
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, abortToken] {
            LOGV2(6104902,
                  "Fetching cluster time key documents from recipient.",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            return _fetchAndStoreRecipientClusterTimeKeyDocs(
                executor, recipientTargeterRS, abortToken);
        })
        .then([this, self = shared_from_this(), executor, abortToken] {
            LOGV2(6104903,
                  "Entering 'data sync' state.",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            return _enterDataSyncState(executor, abortToken);
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, abortToken] {
            LOGV2(6104904,
                  "Waiting for recipient to finish data sync and become consistent.",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            return _waitForRecipientToBecomeConsistentAndEnterBlockingState(
                executor, recipientTargeterRS, abortToken);
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, abortToken, token] {
            LOGV2(6104905,
                  "Waiting for recipient to reach the block timestamp.",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            return _waitForRecipientToReachBlockTimestampAndEnterCommittedState(
                executor, recipientTargeterRS, abortToken, token);
        })
        // Note from here on the migration cannot be aborted, so only the token from the primary
        // only service should be used.
        .onError([this, self = shared_from_this(), executor, token, abortToken](Status status) {
            return _handleErrorOrEnterAbortedState(executor, token, abortToken, status);
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            if (!_stateDoc.getExpireAt()) {
                // Avoid double counting tenant migration statistics after failover.
                // Double counting may still happen if the failover to the same primary
                // happens after this block and before the state doc GC is persisted.
                if (_abortReason) {
                    TenantMigrationStatistics::get(_serviceContext)
                        ->incTotalFailedMigrationsDonated();
                } else {
                    TenantMigrationStatistics::get(_serviceContext)
                        ->incTotalSuccessfulMigrationsDonated();
                }
            }
        })
        .then([this, self = shared_from_this(), executor, token, recipientTargeterRS] {
            return _waitForForgetMigrationThenMarkMigrationGarbageCollectable(
                executor, recipientTargeterRS, token);
        })
        .onCompletion([this,
                       self = shared_from_this(),
                       token,
                       scopedCounter{std::move(scopedOutstandingMigrationCounter)}](Status status) {
            // Don't set the completion promise if the instance has been canceled. We assume
            // whatever canceled the token will also set the promise with an appropriate error.
            checkForTokenInterrupt(token);

            stdx::lock_guard<Latch> lg(_mutex);

            LOGV2(4920400,
                  "Marked migration state as garbage collectable",
                  "migrationId"_attr = _migrationUuid,
                  "expireAt"_attr = _stateDoc.getExpireAt(),
                  "status"_attr = status);

            setPromiseFromStatusIfNotReady(lg, _completionPromise, status);
            LOGV2(5006601,
                  "Tenant migration completed",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId,
                  "status"_attr = status,
                  "abortReason"_attr = _abortReason);
        })
        .semi();
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_enterAbortingIndexBuildsState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const CancellationToken& token) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kUninitialized) {
            return ExecutorFuture(**executor);
        }
    }

    // Enter "abortingIndexBuilds" state.
    return _insertStateDoc(executor, token)
        .then([this, self = shared_from_this(), executor, token](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), token);
        })
        .then([this, self = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();
            pauseTenantMigrationAfterPersistingInitialDonorStateDoc.pauseWhileSet(opCtx);
        });
}

void TenantMigrationDonorService::Instance::_abortIndexBuilds(const CancellationToken& token) {
    checkForTokenInterrupt(token);

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kAbortingIndexBuilds) {
            return;
        }
    }

    // Before starting data sync, abort any in-progress index builds.  No new index
    // builds can start while we are doing this because the mtab prevents it.
    {
        auto opCtxHolder = cc().makeOperationContext();
        auto* opCtx = opCtxHolder.get();
        auto* indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx);
        indexBuildsCoordinator->abortTenantIndexBuilds(
            opCtx, _protocol, _tenantId, "tenant migration");
    }
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_fetchAndStoreRecipientClusterTimeKeyDocs(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& token) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kAbortingIndexBuilds) {
            return ExecutorFuture(**executor);
        }
    }

    return AsyncTry([this, self = shared_from_this(), executor, recipientTargeterRS, token] {
               return recipientTargeterRS->findHost(kPrimaryOnlyReadPreference, token)
                   .thenRunOn(**executor)
                   .then([this, self = shared_from_this(), executor, token](HostAndPort host) {
                       pauseTenantMigrationBeforeFetchingKeys.pauseWhileSet();

                       const auto nss = NamespaceString::kKeysCollectionNamespace;

                       const auto cmdObj = [&] {
                           FindCommandRequest request(NamespaceStringOrUUID{nss});
                           request.setReadConcern(
                               repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern)
                                   .toBSONInner());
                           return request.toBSON(BSONObj());
                       }();

                       auto keyDocs =
                           std::make_shared<std::vector<ExternalKeysCollectionDocument>>();
                       auto fetchStatus = std::make_shared<boost::optional<Status>>();

                       auto fetcherCallback =
                           [this, self = shared_from_this(), fetchStatus, keyDocs](
                               const Fetcher::QueryResponseStatus& dataStatus,
                               Fetcher::NextAction* nextAction,
                               BSONObjBuilder* getMoreBob) {
                               // Throw out any accumulated results on error
                               if (!dataStatus.isOK()) {
                                   *fetchStatus = dataStatus.getStatus();
                                   keyDocs->clear();
                                   return;
                               }

                               const auto& data = dataStatus.getValue();
                               for (const BSONObj& doc : data.documents) {
                                   keyDocs->push_back(
                                       tenant_migration_util::makeExternalClusterTimeKeyDoc(
                                           _migrationUuid, doc.getOwned()));
                               }
                               *fetchStatus = Status::OK();

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
                           RemoteCommandRetryScheduler::makeRetryPolicy<
                               ErrorCategory::RetriableError>(
                               kMaxRecipientKeyDocsFindAttempts,
                               executor::RemoteCommandRequest::kNoTimeout),
                           _sslMode);

                       {
                           stdx::lock_guard<Latch> lg(_mutex);
                           // Note the fetcher cannot be canceled via token, so this check for
                           // interrupt is required otherwise stepdown/shutdown could block waiting
                           // for the fetcher to complete.
                           checkForTokenInterrupt(token);
                           _recipientKeysFetcher = fetcher;
                       }

                       uassertStatusOK(fetcher->schedule());

                       // We use the instance cleanup executor instead of the scoped task executor
                       // here in order to avoid a self-deadlock situation in the Fetcher during
                       // failovers.
                       return fetcher->onCompletion()
                           .thenRunOn(_donorService->getInstanceCleanupExecutor())
                           .then(
                               [this, self = shared_from_this(), fetchStatus, keyDocs, fetcher]() {
                                   {
                                       stdx::lock_guard<Latch> lg(_mutex);
                                       _recipientKeysFetcher.reset();
                                   }

                                   if (!*fetchStatus) {
                                       // The callback never got invoked.
                                       uasserted(
                                           5340400,
                                           "Internal error running cursor callback in command");
                                   }

                                   uassertStatusOK(fetchStatus->get());

                                   return *keyDocs;
                               });
                   })
                   .then([this, self = shared_from_this(), executor, token](auto keyDocs) {
                       checkForTokenInterrupt(token);

                       return tenant_migration_util::storeExternalClusterTimeKeyDocs(
                           std::move(keyDocs));
                   })
                   .then([this, self = shared_from_this(), token](repl::OpTime lastKeyOpTime) {
                       pauseTenantMigrationDonorBeforeWaitingForKeysToReplicate.pauseWhileSet();

                       auto allMembersWriteConcern =
                           WriteConcernOptions(repl::ReplSetConfig::kConfigAllWriteConcernName,
                                               WriteConcernOptions::SyncMode::NONE,
                                               WriteConcernOptions::kNoTimeout);
                       auto writeConcernFuture = repl::ReplicationCoordinator::get(_serviceContext)
                                                     ->awaitReplicationAsyncNoWTimeout(
                                                         lastKeyOpTime, allMembersWriteConcern);
                       return future_util::withCancellation(std::move(writeConcernFuture), token);
                   });
           })
        .until([](Status status) { return shouldStopFetchingRecipientClusterTimeKeyDocs(status); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_enterDataSyncState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {
    pauseTenantMigrationAfterFetchingAndStoringKeys.pauseWhileSet();
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kAbortingIndexBuilds) {
            return ExecutorFuture(**executor);
        }
    }

    pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState.pauseWhileSet();

    // Enter "dataSync" state.
    return _updateStateDoc(executor, TenantMigrationDonorStateEnum::kDataSync, abortToken)
        .then([this, self = shared_from_this(), executor, abortToken](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), abortToken);
        });
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_waitUntilStartMigrationDonorTimestampIsCheckpointed(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {

    if (getProtocol() != MigrationProtocolEnum::kShardMerge) {
        return ExecutorFuture(**executor);
    }

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto startMigrationDonorTimestamp = [&] {
        stdx::lock_guard<Latch> lg(_mutex);
        return *_stateDoc.getStartMigrationDonorTimestamp();
    }();

    invariant(startMigrationDonorTimestamp <= repl::ReplicationCoordinator::get(opCtx)
                                                  ->getCurrentCommittedSnapshotOpTime()
                                                  .getTimestamp());

    // For shard merge, we set startApplyingDonorOpTime timestamp on the recipient to the donor's
    // backup cursor checkpoint timestamp, and startMigrationDonorTimestamp to the timestamp after
    // aborting all index builds. As a result, startApplyingDonorOpTime timestamp can be <
    // startMigrationDonorTimestamp, which means we can erroneously fetch and apply index build
    // operations before startMigrationDonorTimestamp. Trigger a stable checkpoint to ensure that
    // the recipient does not fetch and apply donor index build entries before
    // startMigrationDonorTimestamp.
    return AsyncTry([this, self = shared_from_this(), startMigrationDonorTimestamp] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
               if (storageEngine->getLastStableRecoveryTimestamp() < startMigrationDonorTimestamp) {
                   opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(
                       opCtx,
                       /*stableCheckpoint*/ true);
               }
           })
        .until([this, self = shared_from_this(), startMigrationDonorTimestamp](Status status) {
            uassertStatusOK(status);
            auto storageEngine = _serviceContext->getStorageEngine();
            if (storageEngine->getLastStableRecoveryTimestamp() < startMigrationDonorTimestamp) {
                return false;
            }
            return true;
        })
        .withBackoffBetweenIterations(Backoff(Milliseconds(100), Milliseconds(100)))
        .on(**executor, abortToken);
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_waitForRecipientToBecomeConsistentAndEnterBlockingState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& abortToken) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kDataSync) {
            return ExecutorFuture(**executor);
        }
    }

    return _waitUntilStartMigrationDonorTimestampIsCheckpointed(executor, abortToken)
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, abortToken] {
            return _sendRecipientSyncDataCommand(executor, recipientTargeterRS, abortToken);
        })
        .then([this, self = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();
            pauseTenantMigrationBeforeLeavingDataSyncState.pauseWhileSet(opCtx);
        })
        .then([this, self = shared_from_this(), executor, abortToken] {
            // Enter "blocking" state.
            LOGV2(6104907,
                  "Updating its state doc to enter 'blocking' state.",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            return _updateStateDoc(executor, TenantMigrationDonorStateEnum::kBlocking, abortToken)
                .then([this, self = shared_from_this(), executor, abortToken](repl::OpTime opTime) {
                    return _waitForMajorityWriteConcern(executor, std::move(opTime), abortToken);
                });
        });
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_waitForRecipientToReachBlockTimestampAndEnterCommittedState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& abortToken,
    const CancellationToken& token) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kBlocking) {
            return ExecutorFuture(**executor);
        }

        invariant(_stateDoc.getBlockTimestamp());
    }
    // Source to cancel the timeout if the operation completed in time.
    CancellationSource cancelTimeoutSource;
    CancellationSource recipientSyncDataSource(abortToken);

    auto deadlineReachedFuture =
        (*executor)->sleepFor(Milliseconds(repl::tenantMigrationBlockingStateTimeoutMS.load()),
                              cancelTimeoutSource.token());

    return whenAny(std::move(deadlineReachedFuture),
                   _sendRecipientSyncDataCommand(
                       executor, recipientTargeterRS, recipientSyncDataSource.token()))
        .thenRunOn(**executor)
        .then([this, self = shared_from_this(), cancelTimeoutSource, recipientSyncDataSource](
                  auto result) mutable {
            const auto& [status, idx] = result;

            if (idx == 0) {
                LOGV2(5290301,
                      "Tenant migration blocking stage timeout expired",
                      "timeoutMs"_attr = repl::tenantMigrationBlockingStateTimeoutMS.load());
                // Deadline reached, cancel the pending '_sendRecipientSyncDataCommand()'...
                recipientSyncDataSource.cancel();
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
                [&](const BSONObj& data) {
                    if (!data.hasField("blockTimeMS")) {
                        pauseTenantMigrationBeforeLeavingBlockingState.pauseWhileSet(opCtx);
                    } else {
                        const auto blockTime = Milliseconds{data.getIntField("blockTimeMS")};
                        LOGV2(5010400,
                              "Keep migration in blocking state",
                              "blockTime"_attr = blockTime);
                        opCtx->sleepFor(blockTime);
                    }
                },
                [&](const BSONObj& data) {
                    return !data.hasField("tenantId") || _tenantId == data["tenantId"].str();
                });

            if (MONGO_unlikely(abortTenantMigrationBeforeLeavingBlockingState.shouldFail())) {
                uasserted(ErrorCodes::InternalError, "simulate a tenant migration error");
            }
        })
        .then([this, self = shared_from_this(), executor, abortToken, token] {
            // Last chance to abort
            checkForTokenInterrupt(abortToken);

            // Enter "commit" state.
            LOGV2(6104908,
                  "Entering 'committed' state.",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            // Ignore the abort token once we've entered the committed state
            return _updateStateDoc(executor, TenantMigrationDonorStateEnum::kCommitted, token)
                .then([this, self = shared_from_this(), executor, token](repl::OpTime opTime) {
                    return _waitForMajorityWriteConcern(executor, std::move(opTime), token)
                        .then([this, self = shared_from_this()] {
                            pauseTenantMigrationBeforeLeavingCommittedState.pauseWhileSet();
                            stdx::lock_guard<Latch> lg(_mutex);
                            // If interrupt is called at some point during execution, it is
                            // possible that interrupt() will fulfill the promise before we
                            // do.
                            setPromiseOkIfNotReady(lg, _decisionPromise);
                        });
                });
        });
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_handleErrorOrEnterAbortedState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token,
    const CancellationToken& abortToken,
    Status status) {
    // Don't handle errors if the instance token is canceled to guarantee we don't enter the abort
    // state because of an earlier error from token cancellation.
    checkForTokenInterrupt(token);

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() == TenantMigrationDonorStateEnum::kAborted) {
            // The migration was resumed on stepup and it was already aborted.
            return ExecutorFuture(**executor);
        }
    }

    // Note we must check the parent token has not been canceled so we don't change the error if the
    // abortToken was canceled because of an instance interruption. The checks don't need to be
    // atomic because a token cannot be uncanceled.
    if (abortToken.isCanceled() && !token.isCanceled()) {
        status = Status(ErrorCodes::TenantMigrationAborted, "Aborted due to donorAbortMigration.");
    }

    auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(
        _serviceContext, _tenantId);
    if (!_initialDonorStateDurablePromise.getFuture().isReady() || !mtab) {
        // The migration failed either before or during inserting the state doc. Use the status to
        // fulfill the _initialDonorStateDurablePromise to fail the donorStartMigration command
        // immediately.
        stdx::lock_guard<Latch> lg(_mutex);
        setPromiseErrorIfNotReady(lg, _initialDonorStateDurablePromise, status);

        return ExecutorFuture(**executor);
    } else if (ErrorCodes::isNotPrimaryError(status) || ErrorCodes::isShutdownError(status)) {
        // Don't abort the migration on retriable errors that may have been generated by the local
        // server shutting/stepping down because it can be resumed when the client retries.
        stdx::lock_guard<Latch> lg(_mutex);
        setPromiseErrorIfNotReady(lg, _initialDonorStateDurablePromise, status);

        return ExecutorFuture(**executor);
    } else {
        LOGV2(6104912,
              "Entering 'aborted' state.",
              "migrationId"_attr = _migrationUuid,
              "tenantId"_attr = _tenantId,
              "status"_attr = status);
        // Enter "abort" state.
        _abortReason.emplace(status);
        return _updateStateDoc(executor, TenantMigrationDonorStateEnum::kAborted, token)
            .then([this, self = shared_from_this(), executor, token](repl::OpTime opTime) {
                return _waitForMajorityWriteConcern(executor, std::move(opTime), token)
                    .then([this, self = shared_from_this()] {
                        stdx::lock_guard<Latch> lg(_mutex);
                        // If interrupt is called at some point during execution, it is
                        // possible that interrupt() will fulfill the promise before we do.
                        setPromiseOkIfNotReady(lg, _decisionPromise);
                    });
            });
    }
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_waitForForgetMigrationThenMarkMigrationGarbageCollectable(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& token) {
    LOGV2(6104909,
          "Waiting to receive 'donorForgetMigration' command.",
          "migrationId"_attr = _migrationUuid,
          "tenantId"_attr = _tenantId);
    auto expiredAt = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);
        return _stateDoc.getExpireAt();
    }();

    if (expiredAt) {
        // The migration state has already been marked as garbage collectable. Set the
        // donorForgetMigration promise here since the Instance's destructor has an
        // invariant that _receiveDonorForgetMigrationPromise is ready.
        onReceiveDonorForgetMigration();
        return ExecutorFuture(**executor);
    }

    // Wait for the donorForgetMigration command.
    // If donorAbortMigration has already canceled work, the abortMigrationSource would be
    // canceled and continued usage of the source would lead to incorrect behavior. Thus, we
    // need to use the token after the migration has reached a decision state in order to continue
    // work, such as sending donorForgetMigration, successfully.
    return std::move(_receiveDonorForgetMigrationPromise.getFuture())
        .thenRunOn(**executor)
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, token] {
            LOGV2(6104910,
                  "Waiting for recipientForgetMigration response.",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            return _sendRecipientForgetMigrationCommand(executor, recipientTargeterRS, token);
        })
        .then([this, self = shared_from_this(), executor, token] {
            LOGV2(6104911,
                  "Marking migration as garbage-collectable.",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            // Note marking the keys as garbage collectable is not atomic with marking the
            // state document garbage collectable, so an interleaved failover can lead the
            // keys to be deleted before the state document has an expiration date. This is
            // acceptable because the decision to forget a migration is not reversible.
            return tenant_migration_util::markExternalKeysAsGarbageCollectable(
                _serviceContext,
                executor,
                _donorService->getInstanceCleanupExecutor(),
                _migrationUuid,
                token);
        })
        .then([this, self = shared_from_this(), executor, token] {
            return _markStateDocAsGarbageCollectable(executor, token);
        })
        .then([this, self = shared_from_this(), executor, token](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), token);
        })
        .then([this, self = shared_from_this()] {
            pauseTenantMigrationDonorAfterMarkingStateGarbageCollectable.pauseWhileSet();
        });
}

}  // namespace mongo
