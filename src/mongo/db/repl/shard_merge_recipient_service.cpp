/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/filesystem/operations.hpp>
#include <fmt/format.h>

#include "mongo/base/checked_cast.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/config.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/shard_merge_recipient_service.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_statistics.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_import.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {
namespace repl {
namespace {
using namespace fmt;
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());
constexpr StringData kOplogBufferPrefix = "repl.migration.oplog_"_sd;
constexpr int kBackupCursorFileFetcherRetryAttempts = 10;
constexpr int kCheckpointTsBackupCursorErrorCode = 6929900;
constexpr int kCloseCursorBeforeOpenErrorCode = 50886;

NamespaceString getOplogBufferNs(const UUID& migrationUUID) {
    return NamespaceString::makeGlobalConfigCollection(kOplogBufferPrefix +
                                                       migrationUUID.toString());
}

boost::intrusive_ptr<ExpressionContext> makeExpressionContext(OperationContext* opCtx) {
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;

    // Add kTenantMigrationOplogView, kSessionTransactionsTableNamespace, and kRsOplogNamespace
    // to resolvedNamespaces since they are all used during different pipeline stages.
    resolvedNamespaces[NamespaceString::kTenantMigrationOplogView.coll()] = {
        NamespaceString::kTenantMigrationOplogView, std::vector<BSONObj>()};

    resolvedNamespaces[NamespaceString::kSessionTransactionsTableNamespace.coll()] = {
        NamespaceString::kSessionTransactionsTableNamespace, std::vector<BSONObj>()};

    resolvedNamespaces[NamespaceString::kRsOplogNamespace.coll()] = {
        NamespaceString::kRsOplogNamespace, std::vector<BSONObj>()};

    return make_intrusive<ExpressionContext>(opCtx,
                                             boost::none, /* explain */
                                             false,       /* fromMongos */
                                             false,       /* needsMerge */
                                             true,        /* allowDiskUse */
                                             true,        /* bypassDocumentValidation */
                                             false,       /* isMapReduceCommand */
                                             NamespaceString::kSessionTransactionsTableNamespace,
                                             boost::none, /* runtimeConstants */
                                             nullptr,     /* collator */
                                             MongoProcessInterface::create(opCtx),
                                             std::move(resolvedNamespaces),
                                             boost::none); /* collUUID */
}

// We allow retrying on the following oplog fetcher errors:
// 1) InvalidSyncSource - we cannot sync from the chosen sync source, potentially because the sync
//    source is too stale or there was a network error when connecting to the sync source.
// 2) ShudownInProgress - the current sync source is shutting down
bool isRetriableOplogFetcherError(Status oplogFetcherStatus) {
    return oplogFetcherStatus == ErrorCodes::InvalidSyncSource ||
        oplogFetcherStatus == ErrorCodes::ShutdownInProgress;
}

// We never restart just the oplog fetcher.  If a failure occurs, we restart the whole state machine
// and recover from there.  So the restart decision is always "no".
class OplogFetcherRestartDecisionTenantMigration
    : public OplogFetcher::OplogFetcherRestartDecision {
public:
    ~OplogFetcherRestartDecisionTenantMigration(){};
    bool shouldContinue(OplogFetcher* fetcher, Status status) final {
        return false;
    }
    void fetchSuccessful(OplogFetcher* fetcher) final {}
};

// The oplog fetcher requires some of the methods in DataReplicatorExternalState to operate.
class DataReplicatorExternalStateTenantMigration : public DataReplicatorExternalState {
public:
    // The oplog fetcher is passed its executor directly and does not use the one from the
    // DataReplicatorExternalState.
    executor::TaskExecutor* getTaskExecutor() const final {
        MONGO_UNREACHABLE;
    }
    std::shared_ptr<executor::TaskExecutor> getSharedTaskExecutor() const final {
        MONGO_UNREACHABLE;
    }

    // The oplog fetcher uses the current term and opTime to inform the sync source of term changes.
    // As the term on the donor and the term on the recipient have nothing to do with each other,
    // we do not want to do that.
    OpTimeWithTerm getCurrentTermAndLastCommittedOpTime() final {
        return {OpTime::kUninitializedTerm, OpTime()};
    }

    // Tenant migration does not require the metadata from the oplog query.
    void processMetadata(const rpc::ReplSetMetadata& replMetadata,
                         const rpc::OplogQueryMetadata& oqMetadata) final {}

    // Tenant migration does not change sync source depending on metadata.
    ChangeSyncSourceAction shouldStopFetching(const HostAndPort& source,
                                              const rpc::ReplSetMetadata& replMetadata,
                                              const rpc::OplogQueryMetadata& oqMetadata,
                                              const OpTime& previousOpTimeFetched,
                                              const OpTime& lastOpTimeFetched) const final {
        return ChangeSyncSourceAction::kContinueSyncing;
    }

    // Tenant migration does not re-evaluate sync source on error.
    ChangeSyncSourceAction shouldStopFetchingOnError(const HostAndPort& source,
                                                     const OpTime& lastOpTimeFetched) const final {
        return ChangeSyncSourceAction::kContinueSyncing;
    }

    // The oplog fetcher should never call the rest of the methods.
    std::unique_ptr<OplogBuffer> makeInitialSyncOplogBuffer(OperationContext* opCtx) const final {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<OplogApplier> makeOplogApplier(
        OplogBuffer* oplogBuffer,
        OplogApplier::Observer* observer,
        ReplicationConsistencyMarkers* consistencyMarkers,
        StorageInterface* storageInterface,
        const OplogApplier::Options& options,
        ThreadPool* writerPool) final {
        MONGO_UNREACHABLE;
    };

    virtual StatusWith<ReplSetConfig> getCurrentConfig() const final {
        MONGO_UNREACHABLE;
    }

    StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx) const final {
        MONGO_UNREACHABLE;
    }

    Status storeLocalConfigDocument(OperationContext* opCtx, const BSONObj& config) final {
        MONGO_UNREACHABLE;
    }

    StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx) const final {
        MONGO_UNREACHABLE;
    }

    JournalListener* getReplicationJournalListener() final {
        MONGO_UNREACHABLE;
    }
};

/*
 * Acceptable classes for the 'Target' are AbstractAsyncComponent and RandomAccessOplogBuffer.
 */
template <class Target>
void shutdownTarget(WithLock lk, Target& target) {
    if (target)
        target->shutdown();
}

template <class Target>
void shutdownTargetWithOpCtx(WithLock lk, Target& target, OperationContext* opCtx) {
    if (target)
        target->shutdown(opCtx);
}

template <class Target>
void joinTarget(Target& target) {
    if (target)
        target->join();
}

template <class Promise>
void setPromiseErrorifNotReady(WithLock lk, Promise& promise, Status status) {
    if (promise.getFuture().isReady()) {
        return;
    }

    promise.setError(status);
}

template <class Promise>
void setPromiseOkifNotReady(WithLock lk, Promise& promise) {
    if (promise.getFuture().isReady()) {
        return;
    }

    promise.emplaceValue();
}

template <class Promise, class Value>
void setPromiseValueIfNotReady(WithLock lk, Promise& promise, Value& value) {
    if (promise.getFuture().isReady()) {
        return;
    }

    promise.emplaceValue(value);
}

Timestamp selectRejectReadsBeforeTimestamp(OperationContext* opCtx,
                                           const Timestamp& returnAfterReachingTimestamp,
                                           const OpTime& oplogApplierOpTime) {
    // Don't allow reading before the opTime timestamp of the final write on the recipient
    // associated with cloning the donor's data so the client can't see an inconsistent state. The
    // oplog applier timestamp may be null if no oplog entries were copied, but data may still have
    // been cloned, so use the last applied opTime in that case.
    //
    // Note the cloning writes happen on a separate thread, but the last applied opTime in the
    // replication coordinator is guaranteed to be inclusive of those writes because this function
    // is called after waiting for the _dataConsistentPromise to resolve, which happens after the
    // last write for cloning completes (and all of its WUOW onCommit() handlers).
    auto finalRecipientWriteTimestamp = oplogApplierOpTime.getTimestamp().isNull()
        ? ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime().getTimestamp()
        : oplogApplierOpTime.getTimestamp();

    // Also don't allow reading before the returnAfterReachingTimestamp (aka the blockTimestamp) to
    // prevent readers from possibly seeing data in a point in time snapshot on the recipient that
    // would not have been seen at the same point in time on the donor if the donor's cluster time
    // is ahead of the recipient's.
    return std::max(finalRecipientWriteTimestamp, returnAfterReachingTimestamp);
}


/**
 * Converts migration errors, such as, network errors and cancellation errors to interrupt
 * error status.
 *
 * On migration interrupt, async components will fail with generic network/cancellation
 * errors rather than interrupt error status. When sending the migration command response to
 * donor, we should convert those into real errors so that donor can decide if they need to
 * retry migration commands.
 */
Status overrideMigrationErrToInterruptStatusIfNeeded(
    const UUID& migrationUUID,
    Status status,
    boost::optional<SharedSemiFuture<void>> interruptFuture = boost::none) {
    if (status.isOK())
        return status;

    // Network and cancellation errors can be caused due to migration interrupt so replace those
    // error status with interrupt error status, if set.
    if (ErrorCodes::isCancellationError(status) || ErrorCodes::isNetworkError(status)) {
        boost::optional<Status> newErrStatus;
        if (interruptFuture && interruptFuture->isReady() &&
            !interruptFuture->getNoThrow().isOK()) {
            newErrStatus = interruptFuture->getNoThrow();
        } else if (status == ErrorCodes::CallbackCanceled) {
            // All of our async components don't exit with CallbackCanceled normally unless
            // they are shut down by the instance itself via interrupt. If we get a
            // CallbackCanceled error without an interrupt, it is coming from the service's
            // cancellation token or scoped task executor shutdown on failovers. It is possible
            // for the token to get canceled or scope task executor to shutdown
            // before the instance is interrupted. So we replace the CallbackCanceled error
            // with InterruptedDueToReplStateChange and treat it as a retryable error.
            newErrStatus =
                Status{ErrorCodes::InterruptedDueToReplStateChange, "operation was interrupted"};
        }

        if (newErrStatus) {
            LOGV2(7339701,
                  "Override migration error with interrupt status",
                  "migrationId"_attr = migrationUUID,
                  "error"_attr = status,
                  "interruptStatus"_attr = newErrStatus);
            return *newErrStatus;
        }
    }
    return status;
}
}  // namespace

MONGO_FAIL_POINT_DEFINE(autoRecipientForgetMigrationAbort);
MONGO_FAIL_POINT_DEFINE(fpBeforeMarkingStateDocAsGarbageCollectable);

ShardMergeRecipientService::ShardMergeRecipientService(ServiceContext* const serviceContext)
    : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}

StringData ShardMergeRecipientService::getServiceName() const {
    return kShardMergeRecipientServiceName;
}

NamespaceString ShardMergeRecipientService::getStateDocumentsNS() const {
    return NamespaceString::kShardMergeRecipientsNamespace;
}

ThreadPool::Limits ShardMergeRecipientService::getThreadPoolLimits() const {
    ThreadPool::Limits limits;
    limits.maxThreads = maxTenantMigrationRecipientThreadPoolSize;
    limits.minThreads = minTenantMigrationRecipientThreadPoolSize;
    return limits;
}

void ShardMergeRecipientService::abortAllMigrations(OperationContext* opCtx) {
    LOGV2(7339700, "Aborting all active shard merge recipient instances.");
    auto instances = getAllInstances(opCtx);
    for (auto& instance : instances) {
        auto typedInstance = checked_pointer_cast<ShardMergeRecipientService::Instance>(instance);
        auto status =
            Status(ErrorCodes::TenantMigrationAborted, "Shard merge recipient service interrupted");
        typedInstance->interruptConditionally(status);
    }
}

ExecutorFuture<void> ShardMergeRecipientService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return AsyncTry([this] {
               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               auto status = StorageInterface::get(opCtx)->createCollection(
                   opCtx, getStateDocumentsNS(), CollectionOptions());
               if (!status.isOK() && status != ErrorCodes::NamespaceExists) {
                   uassertStatusOK(status);
               }
               return Status::OK();
           })
        .until([token](Status status) { return status.isOK() || token.isCanceled(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

void ShardMergeRecipientService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialStateDoc,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {

    auto recipientStateDoc =
        ShardMergeRecipientDocument::parse(IDLParserContext("recipientStateDoc"), initialStateDoc);

    // We don't start migration if `startGarbageCollect` is true. So, it's safe to not
    // check the conflicts with other instances.
    //
    // We need this to avoid races, like, delayed 'recipientForgetMigration'with migration decision
    // 'committed' received after the corresponding migration state doc was deleted and another
    // conflicting migration was started.
    if (recipientStateDoc.getStartGarbageCollect()) {
        return;
    }

    for (auto& instance : existingInstances) {
        auto existingTypedInstance =
            checked_cast<const ShardMergeRecipientService::Instance*>(instance);
        auto existingStateDoc = existingTypedInstance->getStateDoc();

        uassert(ErrorCodes::ConflictingOperationInProgress,
                "An existing shard merge is in progress",
                existingStateDoc.getStartGarbageCollect() || existingStateDoc.getExpireAt());
    }
}

std::shared_ptr<PrimaryOnlyService::Instance> ShardMergeRecipientService::constructInstance(
    BSONObj initialStateDoc) {
    return std::make_shared<ShardMergeRecipientService::Instance>(
        _serviceContext, this, initialStateDoc);
}

ShardMergeRecipientService::Instance::Instance(ServiceContext* const serviceContext,
                                               const ShardMergeRecipientService* recipientService,
                                               BSONObj stateDoc)
    : PrimaryOnlyService::TypedInstance<Instance>(),
      _serviceContext(serviceContext),
      _recipientService(recipientService),
      _stateDoc(
          ShardMergeRecipientDocument::parse(IDLParserContext("mergeRecipientStateDoc"), stateDoc)),
      _tenantIds(_stateDoc.getTenantIds()),
      _migrationUuid(_stateDoc.getId()),
      _donorConnectionString(_stateDoc.getDonorConnectionString().toString()),
      _donorUri(uassertStatusOK(MongoURI::parse(_stateDoc.getDonorConnectionString().toString()))),
      _readPreference(_stateDoc.getReadPreference()),
      _recipientCertificateForDonor(_stateDoc.getRecipientCertificateForDonor()),
      _transientSSLParams([&]() -> boost::optional<TransientSSLParams> {
          if (auto recipientCertificate = _stateDoc.getRecipientCertificateForDonor()) {
              invariant(!repl::tenantMigrationDisableX509Auth);
#ifdef MONGO_CONFIG_SSL
              uassert(ErrorCodes::IllegalOperation,
                      "Cannot run shard merge with x509 authentication as SSL is not enabled",
                      getSSLGlobalParams().sslMode.load() != SSLParams::SSLMode_disabled);
              auto recipientSSLClusterPEMPayload =
                  recipientCertificate->getCertificate().toString() + "\n" +
                  recipientCertificate->getPrivateKey().toString();
              return TransientSSLParams{_donorUri.connectionString(),
                                        std::move(recipientSSLClusterPEMPayload)};
#else
              // If SSL is not supported, the recipientSyncData command should have failed
              // certificate field validation.
              MONGO_UNREACHABLE;
#endif
          } else {
              invariant(repl::tenantMigrationDisableX509Auth);
              return boost::none;
          }
      }()) {
}

boost::optional<BSONObj> ShardMergeRecipientService::Instance::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder bob;

    stdx::lock_guard lk(_mutex);
    bob.append("desc", "shard merge recipient");
    _migrationUuid.appendToBuilder(&bob, "instanceID"_sd);
    {
        BSONArrayBuilder arrayBuilder(bob.subarrayStart("tenantIds"));
        for (const auto& tenantId : _stateDoc.getTenantIds()) {
            tenantId.serializeToBSON(&arrayBuilder);
        }
    }

    bob.append("donorConnectionString", _stateDoc.getDonorConnectionString());
    bob.append("readPreference", _stateDoc.getReadPreference().toInnerBSON());
    bob.append("state", ShardMergeRecipientState_serializer(_stateDoc.getState()));
    bob.appendBool("migrationStarted", !_stateDoc.getStartGarbageCollect());
    bob.append("migrationCompleted", _migrationCompletionPromise.getFuture().isReady());
    bob.append("garbageCollectable", _forgetMigrationDurablePromise.getFuture().isReady());

    if (_stateDoc.getStartFetchingDonorOpTime())
        _stateDoc.getStartFetchingDonorOpTime()->append(&bob, "startFetchingDonorOpTime");
    if (_stateDoc.getStartApplyingDonorOpTime())
        _stateDoc.getStartApplyingDonorOpTime()->append(&bob, "startApplyingDonorOpTime");
    if (_stateDoc.getCloneFinishedRecipientOpTime())
        _stateDoc.getCloneFinishedRecipientOpTime()->append(&bob, "cloneFinishedRecipientOpTime");

    if (_stateDoc.getExpireAt())
        bob.append("expireAt", *_stateDoc.getExpireAt());

    if (_client) {
        bob.append("donorSyncSource", _client->getServerAddress());
    }

    if (_stateDoc.getStartAt()) {
        bob.append("receiveStart", *_stateDoc.getStartAt());
    }

    if (_tenantOplogApplier) {
        bob.appendNumber("numOpsApplied",
                         static_cast<long long>(_tenantOplogApplier->getNumOpsApplied()));
    }

    return bob.obj();
}

void ShardMergeRecipientService::Instance::checkIfOptionsConflict(const BSONObj& options) const {
    auto stateDoc =
        ShardMergeRecipientDocument::parse(IDLParserContext("recipientStateDoc"), options);

    invariant(stateDoc.getId() == _migrationUuid);

    if (stateDoc.getTenantIds() != _tenantIds ||
        stateDoc.getDonorConnectionString() != _donorConnectionString ||
        !stateDoc.getReadPreference().equals(_readPreference) ||
        stateDoc.getRecipientCertificateForDonor() != _recipientCertificateForDonor) {
        uasserted(ErrorCodes::ConflictingOperationInProgress,
                  str::stream() << "Found active migration for migrationId \""
                                << _migrationUuid.toBSON() << "\" with different options "
                                << tenant_migration_util::redactStateDoc(getStateDoc().toBSON()));
    }
}

OpTime ShardMergeRecipientService::Instance::waitUntilMigrationReachesConsistentState(
    OperationContext* opCtx) const {
    return _dataConsistentPromise.getFuture().get(opCtx);
}

OpTime ShardMergeRecipientService::Instance::waitUntilMigrationReachesReturnAfterReachingTimestamp(
    OperationContext* opCtx, const Timestamp& returnAfterReachingTimestamp) {
    // This gives assurance that _tenantOplogApplier pointer won't be empty, and that it has been
    // started. Additionally, we must have finished processing the recipientSyncData command that
    // waits on _dataConsistentPromise.
    _dataConsistentPromise.getFuture().get(opCtx);

    auto getWaitOpTimeFuture = [&]() {
        stdx::unique_lock lk(_mutex);
        // We start tenant oplog applier after recipient informs donor,
        // the data is in consistent state. So, there is a possibility, recipient might receive
        // recipientSyncData cmd with `returnAfterReachingDonorTimestamp` from donor before the
        // recipient has started the tenant oplog applier.
        opCtx->waitForConditionOrInterrupt(_oplogApplierReadyCondVar, lk, [&] {
            return _oplogApplierReady || _migrationCompletionPromise.getFuture().isReady();
        });
        if (_migrationCompletionPromise.getFuture().isReady()) {
            // When the data sync is done, we reset _tenantOplogApplier, so just throw the data sync
            // completion future result.
            _migrationCompletionPromise.getFuture().get();
            MONGO_UNREACHABLE;
        }

        // Sanity checks.
        invariant(_tenantOplogApplier);
        auto state = _stateDoc.getState();
        uassert(
            ErrorCodes::IllegalOperation,
            str::stream()
                << "Failed to wait for the donor timestamp to be majority committed due to"
                   "conflicting tenant migration state, migration uuid: "
                << getMigrationUUID() << " , current state: "
                << ShardMergeRecipientState_serializer(state) << " , expected state: "
                << ShardMergeRecipientState_serializer(ShardMergeRecipientStateEnum::kConsistent)
                << ".",
            state == ShardMergeRecipientStateEnum::kConsistent);

        return _tenantOplogApplier->getNotificationForOpTime(
            OpTime(returnAfterReachingTimestamp, OpTime::kUninitializedTerm));
    };

    auto waitOpTimeFuture = getWaitOpTimeFuture();
    fpWaitUntilTimestampMajorityCommitted.pauseWhileSet();
    auto swDonorRecipientOpTimePair = waitOpTimeFuture.getNoThrow();

    auto status = swDonorRecipientOpTimePair.getStatus();

    // A cancellation error may occur due to an interrupt. If that is the case, replace the error
    // code with the interrupt code, the true reason for interruption.
    status = overrideMigrationErrToInterruptStatusIfNeeded(
        _migrationUuid, status, _interruptPromise.getFuture());
    uassertStatusOK(status);

    auto& donorRecipientOpTimePair = swDonorRecipientOpTimePair.getValue();

    // Make sure that the recipient logical clock has advanced to at least the donor timestamp
    // before returning success for recipientSyncData.
    // Note: tickClusterTimeTo() will not tick the recipient clock backwards in time.
    VectorClockMutable::get(opCtx)->tickClusterTimeTo(LogicalTime(returnAfterReachingTimestamp));

    stdx::unique_lock lk(_mutex);
    _stateDoc.setRejectReadsBeforeTimestamp(selectRejectReadsBeforeTimestamp(
        opCtx, returnAfterReachingTimestamp, donorRecipientOpTimePair.recipientOpTime));
    const auto stateDoc = _stateDoc;
    lk.unlock();
    _stopOrHangOnFailPoint(&fpBeforePersistingRejectReadsBeforeTimestamp, opCtx);

    auto lastOpBeforeUpdate = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    _writeStateDoc(opCtx, stateDoc, OpType::kUpdate);
    auto lastOpAfterUpdate = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    auto replCoord = repl::ReplicationCoordinator::get(_serviceContext);
    if (lastOpBeforeUpdate == lastOpAfterUpdate) {
        // updateStateDoc was a no-op, but we still must ensure it's all-replicated.
        lastOpAfterUpdate = uassertStatusOK(replCoord->getLatestWriteOpTime(opCtx));
        LOGV2(7339702,
              "Fixed write timestamp for recording rejectReadsBeforeTimestamp",
              "newWriteOpTime"_attr = lastOpAfterUpdate);
    }

    WriteConcernOptions writeConcern(repl::ReplSetConfig::kConfigAllWriteConcernName,
                                     WriteConcernOptions::SyncMode::NONE,
                                     opCtx->getWriteConcern().wTimeout);
    uassertStatusOK(replCoord->awaitReplication(opCtx, lastOpAfterUpdate, writeConcern).status);

    _stopOrHangOnFailPoint(&fpAfterWaitForRejectReadsBeforeTimestamp, opCtx);

    return donorRecipientOpTimePair.donorOpTime;
}

std::unique_ptr<DBClientConnection> ShardMergeRecipientService::Instance::_connectAndAuth(
    const HostAndPort& serverAddress, StringData applicationName) {
    auto swClientBase = ConnectionString(serverAddress)
                            .connect(applicationName,
                                     0 /* socketTimeout */,
                                     nullptr /* uri */,
                                     nullptr /* apiParameters */,
                                     _transientSSLParams ? &_transientSSLParams.value() : nullptr);
    if (!swClientBase.isOK()) {
        LOGV2_ERROR(7339719,
                    "Failed to connect to migration donor",
                    "migrationId"_attr = getMigrationUUID(),
                    "serverAddress"_attr = serverAddress,
                    "applicationName"_attr = applicationName,
                    "error"_attr = swClientBase.getStatus());
        uassertStatusOK(swClientBase.getStatus());
    }

    auto clientBase = swClientBase.getValue().release();

    // ConnectionString::connect() always returns a DBClientConnection in a unique_ptr of
    // DBClientBase type.
    std::unique_ptr<DBClientConnection> client(checked_cast<DBClientConnection*>(clientBase));

    // Authenticate connection to the donor.
    if (!_transientSSLParams) {
        uassertStatusOK(replAuthenticate(clientBase)
                            .withContext(str::stream()
                                         << "ShardMergeRecipientService failed to authenticate to "
                                         << serverAddress));
    } else if (MONGO_likely(!skipTenantMigrationRecipientAuth.shouldFail())) {
        client->auth(auth::createInternalX509AuthDocument());
    }

    return client;
}

OpTime ShardMergeRecipientService::Instance::_getDonorMajorityOpTime(
    std::unique_ptr<mongo::DBClientConnection>& client) {
    auto oplogOpTimeFields =
        BSON(OplogEntry::kTimestampFieldName << 1 << OplogEntry::kTermFieldName << 1);
    FindCommandRequest findCmd{NamespaceString::kRsOplogNamespace};
    findCmd.setSort(BSON("$natural" << -1));
    findCmd.setProjection(oplogOpTimeFields);
    findCmd.setReadConcern(ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    auto majorityOpTimeBson = client->findOne(
        std::move(findCmd), ReadPreferenceSetting{ReadPreference::SecondaryPreferred});
    uassert(7339780, "Found no entries in the remote oplog", !majorityOpTimeBson.isEmpty());

    auto majorityOpTime = uassertStatusOK(OpTime::parseFromOplogEntry(majorityOpTimeBson));
    return majorityOpTime;
}

SemiFuture<ShardMergeRecipientService::Instance::ConnectionPair>
ShardMergeRecipientService::Instance::_createAndConnectClients() {
    LOGV2_DEBUG(7339720,
                1,
                "Recipient migration instance connecting clients",
                "migrationId"_attr = getMigrationUUID(),
                "connectionString"_attr = _donorConnectionString,
                "readPreference"_attr = _readPreference);
    const auto& servers = _donorUri.getServers();
    stdx::lock_guard lk(_mutex);
    _donorReplicaSetMonitor = ReplicaSetMonitor::createIfNeeded(
        _donorUri.getSetName(), std::set<HostAndPort>(servers.begin(), servers.end()));

    // Only ever used to cancel when the setTenantMigrationRecipientInstanceHostTimeout failpoint is
    // set.
    CancellationSource getHostCancelSource;
    setTenantMigrationRecipientInstanceHostTimeout.execute([&](const BSONObj& data) {
        auto exec = **_scopedExecutor;
        const auto deadline =
            exec->now() + Milliseconds(data["findHostTimeoutMillis"].safeNumberLong());
        // Cancel the find host request after a timeout. Ignore callback handle.
        exec->sleepUntil(deadline, CancellationToken::uncancelable())
            .getAsync([getHostCancelSource](auto) mutable { getHostCancelSource.cancel(); });
    });

    if (MONGO_unlikely(hangAfterCreatingRSM.shouldFail())) {
        LOGV2(7339703, "hangAfterCreatingRSM failpoint enabled");
        hangAfterCreatingRSM.pauseWhileSet();
    }

    const auto kDelayedMajorityOpTimeErrorCode = 5272000;
    return AsyncTry([this,
                     self = shared_from_this(),
                     getHostCancelSource,
                     kDelayedMajorityOpTimeErrorCode] {
               stdx::lock_guard lk(_mutex);

               // Get all donor hosts that we have excluded.
               const auto& excludedHosts = _getExcludedDonorHosts(lk);
               return _donorReplicaSetMonitor
                   ->getHostOrRefresh(_readPreference, excludedHosts, getHostCancelSource.token())
                   .thenRunOn(**_scopedExecutor)
                   .then([this, self = shared_from_this(), kDelayedMajorityOpTimeErrorCode](
                             const HostAndPort& serverAddress) {
                       LOGV2(7339704,
                             "Attempting to connect to donor host",
                             "donorHost"_attr = serverAddress,
                             "migrationId"_attr = getMigrationUUID());
                       auto applicationName = "TenantMigration_" + getMigrationUUID().toString();

                       auto client = _connectAndAuth(serverAddress, applicationName);

                       boost::optional<repl::OpTime> startApplyingOpTime;
                       Timestamp startMigrationDonorTimestamp;
                       {
                           stdx::lock_guard lk(_mutex);
                           startApplyingOpTime = _stateDoc.getStartApplyingDonorOpTime();
                           startMigrationDonorTimestamp =
                               _stateDoc.getStartMigrationDonorTimestamp();
                       }

                       auto majoritySnapshotOpTime = _getDonorMajorityOpTime(client);
                       if (majoritySnapshotOpTime.getTimestamp() < startMigrationDonorTimestamp) {
                           stdx::lock_guard lk(_mutex);
                           const auto now = _serviceContext->getFastClockSource()->now();
                           _excludeDonorHost(
                               lk,
                               serverAddress,
                               now + Milliseconds(tenantMigrationExcludeDonorHostTimeoutMS));
                           uasserted(
                               kDelayedMajorityOpTimeErrorCode,
                               str::stream()
                                   << "majoritySnapshotOpTime on donor host must not be behind "
                                      "startMigrationDonorTimestamp, majoritySnapshotOpTime: "
                                   << majoritySnapshotOpTime.toString()
                                   << "; startMigrationDonorTimestamp: "
                                   << startMigrationDonorTimestamp.toString());
                       }
                       if (startApplyingOpTime && majoritySnapshotOpTime < *startApplyingOpTime) {
                           stdx::lock_guard lk(_mutex);
                           const auto now = _serviceContext->getFastClockSource()->now();
                           _excludeDonorHost(
                               lk,
                               serverAddress,
                               now + Milliseconds(tenantMigrationExcludeDonorHostTimeoutMS));
                           uasserted(
                               kDelayedMajorityOpTimeErrorCode,
                               str::stream()
                                   << "majoritySnapshotOpTime on donor host must not be behind "
                                      "startApplyingDonorOpTime, majoritySnapshotOpTime: "
                                   << majoritySnapshotOpTime.toString()
                                   << "; startApplyingDonorOpTime: "
                                   << (*startApplyingOpTime).toString());
                       }

                       applicationName += "_oplogFetcher";
                       auto oplogFetcherClient = _connectAndAuth(serverAddress, applicationName);
                       return ConnectionPair(std::move(client), std::move(oplogFetcherClient));
                   });
           })
        .until([this, self = shared_from_this(), kDelayedMajorityOpTimeErrorCode](
                   const StatusWith<ConnectionPair>& statusWith) {
            auto status = statusWith.getStatus();

            if (status.isOK()) {
                return true;
            }

            LOGV2_ERROR(7339721,
                        "Connecting to donor failed",
                        "migrationId"_attr = getMigrationUUID(),
                        "error"_attr = status);

            // If the future chain has been interrupted, stop retrying.
            if (!_getInterruptStatus().isOK()) {
                return true;
            }

            if (MONGO_unlikely(skipRetriesWhenConnectingToDonorHost.shouldFail())) {
                LOGV2(7339705,
                      "skipRetriesWhenConnectingToDonorHost failpoint enabled, migration "
                      "proceeding with error from connecting to sync source");
                return true;
            }

            /*
             * Retry sync source selection if we encountered any of the following errors:
             * 1) The RSM couldn't find a suitable donor host
             * 2) The majority snapshot OpTime on the donor host was not ahead of our stored
             * 'startApplyingDonorOpTime'
             * 3) Some other retriable error
             */
            if (status == ErrorCodes::FailedToSatisfyReadPreference ||
                status == ErrorCodes::Error(kDelayedMajorityOpTimeErrorCode) ||
                ErrorCodes::isRetriableError(status)) {
                return false;
            }

            return true;
        })
        .on(**_scopedExecutor, CancellationToken::uncancelable())
        .semi();
}

void ShardMergeRecipientService::Instance::_excludeDonorHost(WithLock,
                                                             const HostAndPort& host,
                                                             Date_t until) {
    LOGV2_DEBUG(7339722,
                2,
                "Excluding donor host",
                "donorHost"_attr = host,
                "until"_attr = until.toString());

    _excludedDonorHosts.emplace_back(std::make_pair(host, until));
}

std::vector<HostAndPort> ShardMergeRecipientService::Instance::_getExcludedDonorHosts(WithLock lk) {
    const auto now = _serviceContext->getFastClockSource()->now();

    // Clean up any hosts that have had their exclusion duration expired.
    auto itr = std::remove_if(
        _excludedDonorHosts.begin(),
        _excludedDonorHosts.end(),
        [now](const std::pair<HostAndPort, Date_t>& pair) { return pair.second < now; });
    _excludedDonorHosts.erase(itr, _excludedDonorHosts.end());

    // Return the list of currently excluded donor hosts.
    std::vector<HostAndPort> excludedHosts;
    std::transform(_excludedDonorHosts.begin(),
                   _excludedDonorHosts.end(),
                   std::back_inserter(excludedHosts),
                   [](const std::pair<HostAndPort, Date_t>& pair) { return pair.first; });
    return excludedHosts;
}

SemiFuture<void> ShardMergeRecipientService::Instance::_initializeAndDurablyPersistStateDoc() {
    stdx::unique_lock lk(_mutex);
    uassert(ErrorCodes::TenantMigrationForgotten,
            str::stream() << "Migration " << getMigrationUUID()
                          << " already marked for garbage collection",
            !(_isCommitOrAbortState(lk) || _stateDoc.getStartGarbageCollect()));

    uassert(ErrorCodes::TenantMigrationAborted,
            str::stream() << "Failover happened during migration :: migrationId: "
                          << getMigrationUUID(),
            !_stateDoc.getStartAt());

    LOGV2_DEBUG(7339723,
                2,
                "Recipient migration instance initializing state document",
                "migrationId"_attr = getMigrationUUID(),
                "connectionString"_attr = _donorConnectionString,
                "readPreference"_attr = _readPreference);

    // Record the time at which the state doc is initialized.
    _stateDoc.setStartAt(_serviceContext->getFastClockSource()->now());

    if (MONGO_unlikely(failWhilePersistingTenantMigrationRecipientInstanceStateDoc.shouldFail())) {
        LOGV2(7339706, "Persisting state doc failed due to fail point enabled.");
        uasserted(ErrorCodes::NotWritablePrimary,
                  "Persisting state doc failed for shard merge recipient service - "
                  "'failWhilePersistingTenantMigrationRecipientInstanceStateDoc' fail point "
                  "active");
    }

    auto failToInitializeChangeCbk = [&](OperationContext* opCtx) {
        opCtx->recoveryUnit()->onRollback([&](auto _) {
            stdx::unique_lock lk(_mutex);
            _stateDoc.setStartGarbageCollect(true);
        });
    };

    return _insertStateDocForMajority(lk, failToInitializeChangeCbk);
}

SemiFuture<void> ShardMergeRecipientService::Instance::_killBackupCursor() {
    stdx::lock_guard lk(_mutex);

    auto& donorBackupCursorInfo = _getDonorBackupCursorInfo(lk);
    if (donorBackupCursorInfo.cursorId <= 0) {
        return SemiFuture<void>::makeReady();
    }

    if (_backupCursorKeepAliveFuture) {
        _backupCursorKeepAliveCancellation.cancel();
    }

    return std::exchange(_backupCursorKeepAliveFuture, {})
        .value_or(SemiFuture<void>::makeReady())
        .thenRunOn(_recipientService->getInstanceCleanupExecutor())
        .then([this, self = shared_from_this(), donorBackupCursorInfo] {
            LOGV2_INFO(7339724,
                       "Killing backup cursor",
                       "migrationId"_attr = getMigrationUUID(),
                       "cursorId"_attr = donorBackupCursorInfo.cursorId);

            stdx::lock_guard lk(_mutex);
            executor::RemoteCommandRequest request(
                _client->getServerHostAndPort(),
                donorBackupCursorInfo.nss.db().toString(),
                BSON("killCursors" << donorBackupCursorInfo.nss.coll().toString() << "cursors"
                                   << BSON_ARRAY(donorBackupCursorInfo.cursorId)),
                nullptr);
            request.sslMode = _donorUri.getSSLMode();

            const auto scheduleResult = _scheduleKillBackupCursorWithLock(
                lk, _recipientService->getInstanceCleanupExecutor());
            if (!scheduleResult.isOK()) {
                LOGV2_WARNING(7339725,
                              "Failed to run killCursors command on backup cursor",
                              "status"_attr = scheduleResult.getStatus());
            }
        })
        .semi();
}

SemiFuture<void> ShardMergeRecipientService::Instance::_openBackupCursor(
    const CancellationToken& token) {

    const auto aggregateCommandRequestObj = [] {
        AggregateCommandRequest aggRequest(
            NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
            {BSON("$backupCursor" << BSONObj())});
        // We must set a writeConcern on internal commands.
        aggRequest.setWriteConcern(WriteConcernOptions());
        return aggRequest.toBSON(BSONObj());
    }();

    stdx::lock_guard lk(_mutex);
    uassertStatusOK(_getInterruptStatus());

    LOGV2_DEBUG(7339726,
                1,
                "Trying to open backup cursor on donor primary",
                "migrationId"_attr = _stateDoc.getId(),
                "donorConnectionString"_attr = _stateDoc.getDonorConnectionString());

    const auto startMigrationDonorTimestamp = _stateDoc.getStartMigrationDonorTimestamp();

    auto fetchStatus = std::make_shared<boost::optional<Status>>();
    auto uniqueMetadataInfo = std::make_unique<boost::optional<shard_merge_utils::MetadataInfo>>();
    const auto fetcherCallback = [this,
                                  self = shared_from_this(),
                                  fetchStatus,
                                  metadataInfoPtr = uniqueMetadataInfo.get(),
                                  token,
                                  startMigrationDonorTimestamp](
                                     const Fetcher::QueryResponseStatus& dataStatus,
                                     Fetcher::NextAction* nextAction,
                                     BSONObjBuilder* getMoreBob) noexcept {
        try {
            uassertStatusOK(dataStatus);
            uassert(ErrorCodes::CallbackCanceled, "backup cursor interrupted", !token.isCanceled());

            const auto uniqueOpCtx = cc().makeOperationContext();
            const auto opCtx = uniqueOpCtx.get();

            const auto& data = dataStatus.getValue();
            for (const BSONObj& doc : data.documents) {
                if (doc["metadata"]) {
                    // First batch must contain the metadata.
                    const auto& metadata = doc["metadata"].Obj();
                    auto checkpointTimestamp = metadata["checkpointTimestamp"].timestamp();

                    LOGV2_INFO(7339727,
                               "Opened backup cursor on donor",
                               "migrationId"_attr = getMigrationUUID(),
                               "backupCursorId"_attr = data.cursorId,
                               "backupCursorCheckpointTimestamp"_attr = checkpointTimestamp);

                    {
                        stdx::lock_guard lk(_mutex);
                        stdx::lock_guard<TenantMigrationSharedData> sharedDatalk(*_sharedData);
                        _sharedData->setDonorBackupCursorInfo(
                            sharedDatalk,
                            BackupCursorInfo{data.cursorId, data.nss, checkpointTimestamp});
                    }

                    // This ensures that the recipient won’t receive any 2 phase index build donor
                    // oplog entries during the migration. We also have a check in the tenant oplog
                    // applier to detect such oplog entries. Adding a check here helps us to detect
                    // the problem earlier.
                    uassert(kCheckpointTsBackupCursorErrorCode,
                            "backupCursorCheckpointTimestamp should be greater than or equal to "
                            "startMigrationDonorTimestamp",
                            checkpointTimestamp >= startMigrationDonorTimestamp);

                    invariant(metadataInfoPtr && !*metadataInfoPtr);
                    (*metadataInfoPtr) = shard_merge_utils::MetadataInfo::constructMetadataInfo(
                        getMigrationUUID(), _client->getServerAddress(), metadata);
                } else {
                    LOGV2_DEBUG(7339728,
                                1,
                                "Backup cursor entry",
                                "migrationId"_attr = getMigrationUUID(),
                                "filename"_attr = doc["filename"].String(),
                                "backupCursorId"_attr = data.cursorId);

                    invariant(metadataInfoPtr && *metadataInfoPtr);
                    auto docs =
                        std::vector<mongo::BSONObj>{(*metadataInfoPtr)->toBSON(doc).getOwned()};

                    // Disabling internal document validation because the fetcher batch size
                    // can exceed the max data size limit BSONObjMaxUserSize with the
                    // additional fields we add to documents.
                    DisableDocumentValidation documentValidationDisabler(
                        opCtx, DocumentValidationSettings::kDisableInternalValidation);

                    write_ops::InsertCommandRequest insertOp(
                        shard_merge_utils::getDonatedFilesNs(getMigrationUUID()));
                    insertOp.setDocuments(std::move(docs));
                    insertOp.setWriteCommandRequestBase([] {
                        write_ops::WriteCommandRequestBase wcb;
                        wcb.setOrdered(true);
                        return wcb;
                    }());

                    auto writeResult = write_ops_exec::performInserts(opCtx, insertOp);
                    invariant(!writeResult.results.empty());
                    // Writes are ordered, check only the last writeOp result.
                    uassertStatusOK(writeResult.results.back());
                }
            }

            *fetchStatus = Status::OK();
            if (!getMoreBob || data.documents.empty()) {
                // Exit fetcher but keep the backupCursor alive to prevent WT on Donor from
                // modifying file bytes. backupCursor can be closed after all Recipient nodes
                // have copied files from Donor primary.
                *nextAction = Fetcher::NextAction::kExitAndKeepCursorAlive;
                return;
            }

            getMoreBob->append("getMore", data.cursorId);
            getMoreBob->append("collection", data.nss.coll());
        } catch (DBException& ex) {
            LOGV2_ERROR(7339729,
                        "Error fetching backup cursor entries",
                        "migrationId"_attr = getMigrationUUID(),
                        "error"_attr = ex.toString());
            *fetchStatus = ex.toStatus();
        }
    };

    _donorFilenameBackupCursorFileFetcher = std::make_unique<Fetcher>(
        _backupCursorExecutor.get(),
        _client->getServerHostAndPort(),
        DatabaseName::kAdmin.toString(),
        aggregateCommandRequestObj,
        fetcherCallback,
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred).toContainingBSON(),
        executor::RemoteCommandRequest::kNoTimeout, /* aggregateTimeout */
        executor::RemoteCommandRequest::kNoTimeout, /* getMoreNetworkTimeout */
        RemoteCommandRetryScheduler::makeRetryPolicy<ErrorCategory::RetriableError>(
            kBackupCursorFileFetcherRetryAttempts, executor::RemoteCommandRequest::kNoTimeout),
        transport::kGlobalSSLMode);

    uassertStatusOK(_donorFilenameBackupCursorFileFetcher->schedule());

    return _donorFilenameBackupCursorFileFetcher->onCompletion()
        .thenRunOn(**_scopedExecutor)
        .then([fetchStatus, uniqueMetadataInfo = std::move(uniqueMetadataInfo)] {
            if (!*fetchStatus) {
                // the callback was never invoked
                uasserted(7339781, "Internal error running cursor callback in command");
            }

            uassertStatusOK(fetchStatus->get());
        })
        .semi();
}

StatusWith<executor::TaskExecutor::CallbackHandle>
ShardMergeRecipientService::Instance::_scheduleKillBackupCursorWithLock(
    WithLock lk, std::shared_ptr<executor::TaskExecutor> executor) {
    auto& donorBackupCursorInfo = _getDonorBackupCursorInfo(lk);
    executor::RemoteCommandRequest killCursorsRequest(
        _client->getServerHostAndPort(),
        donorBackupCursorInfo.nss.db().toString(),
        BSON("killCursors" << donorBackupCursorInfo.nss.coll().toString() << "cursors"
                           << BSON_ARRAY(donorBackupCursorInfo.cursorId)),
        nullptr);
    killCursorsRequest.sslMode = _donorUri.getSSLMode();

    return executor->scheduleRemoteCommand(
        killCursorsRequest, [](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            if (!args.response.isOK()) {
                LOGV2_WARNING(7339730,
                              "killCursors command task failed",
                              "error"_attr = redact(args.response.status));
                return;
            }
            auto status = getStatusFromCommandResult(args.response.data);
            if (status.isOK()) {
                LOGV2_INFO(7339731, "Killed backup cursor");
            } else {
                LOGV2_WARNING(7339732, "killCursors command failed", "error"_attr = redact(status));
            }
        });
}

SemiFuture<void> ShardMergeRecipientService::Instance::_openBackupCursorWithRetry(
    const CancellationToken& token) {
    return AsyncTry([this, self = shared_from_this(), token] { return _openBackupCursor(token); })
        .until([this, self = shared_from_this()](Status status) {
            if (status == ErrorCodes::BackupCursorOpenConflictWithCheckpoint) {
                LOGV2_INFO(7339733,
                           "Retrying backup cursor creation after transient error",
                           "migrationId"_attr = getMigrationUUID(),
                           "status"_attr = status);

                return false;
            } else if (status.code() == kCheckpointTsBackupCursorErrorCode ||
                       status.code() == kCloseCursorBeforeOpenErrorCode) {
                LOGV2_INFO(7339734,
                           "Closing backup cursor and retrying after getting retryable error",
                           "migrationId"_attr = getMigrationUUID(),
                           "status"_attr = status);

                stdx::lock_guard lk(_mutex);
                const auto scheduleResult =
                    _scheduleKillBackupCursorWithLock(lk, _backupCursorExecutor);
                uassertStatusOK(scheduleResult);

                return false;
            }

            return true;
        })
        .on(**_scopedExecutor, token)
        .semi();
}

const BackupCursorInfo& ShardMergeRecipientService::Instance::_getDonorBackupCursorInfo(
    WithLock) const {
    stdx::lock_guard<TenantMigrationSharedData> sharedDatalk(*_sharedData);
    return _sharedData->getDonorBackupCursorInfo(sharedDatalk);
}

void ShardMergeRecipientService::Instance::_keepBackupCursorAlive(const CancellationToken& token) {
    LOGV2_DEBUG(7339735,
                1,
                "Starting periodic 'getMore' requests to keep "
                "backup cursor alive.",
                "migrationId"_attr = getMigrationUUID());

    stdx::lock_guard lk(_mutex);
    _backupCursorKeepAliveCancellation = CancellationSource(token);
    auto& donorBackupCursorInfo = _getDonorBackupCursorInfo(lk);
    _backupCursorKeepAliveFuture =
        shard_merge_utils::keepBackupCursorAlive(_backupCursorKeepAliveCancellation,
                                                 _backupCursorExecutor,
                                                 _client->getServerHostAndPort(),
                                                 donorBackupCursorInfo.cursorId,
                                                 donorBackupCursorInfo.nss);
}

SemiFuture<void> ShardMergeRecipientService::Instance::_enterLearnedFilenamesState() {
    stdx::lock_guard lk(_mutex);
    _stateDoc.setState(ShardMergeRecipientStateEnum::kLearnedFilenames);
    return _updateStateDocForMajority(lk);
}

boost::optional<OpTime> ShardMergeRecipientService::Instance::_getOldestActiveTransactionAt(
    Timestamp ReadTimestamp) {
    const auto preparedState = DurableTxnState_serializer(DurableTxnStateEnum::kPrepared);
    const auto inProgressState = DurableTxnState_serializer(DurableTxnStateEnum::kInProgress);
    auto transactionTableOpTimeFields = BSON(SessionTxnRecord::kStartOpTimeFieldName << 1);

    FindCommandRequest findCmd{NamespaceString::kSessionTransactionsTableNamespace};
    findCmd.setFilter(BSON("state" << BSON("$in" << BSON_ARRAY(preparedState << inProgressState))));
    findCmd.setSort(BSON(SessionTxnRecord::kStartOpTimeFieldName.toString() << 1));
    findCmd.setProjection(transactionTableOpTimeFields);
    // Generally, snapshot reads on config.transactions table have some risks.
    // But for this case, it is safe because we query only for multi-statement transaction entries
    // (and "state" field is set only for multi-statement transaction transactions) and writes to
    // config.transactions collection aren't coalesced for multi-statement transactions during
    // secondary oplog application, unlike the retryable writes where updates to config.transactions
    // collection are coalesced on secondaries.
    findCmd.setReadConcern(BSON(repl::ReadConcernArgs::kLevelFieldName
                                << repl::readConcernLevels::kSnapshotName
                                << repl::ReadConcernArgs::kAtClusterTimeFieldName << ReadTimestamp
                                << repl::ReadConcernArgs::kAllowTransactionTableSnapshot << true));

    auto earliestOpenTransactionBson = _client->findOne(std::move(findCmd), _readPreference);
    LOGV2_DEBUG(7339736,
                2,
                "Transaction table entry for earliest transaction that was open at the read "
                "concern majority optime on remote node (may be empty)",
                "migrationId"_attr = getMigrationUUID(),
                "earliestOpenTransaction"_attr = earliestOpenTransactionBson);

    boost::optional<OpTime> startOpTime;
    if (!earliestOpenTransactionBson.isEmpty()) {
        auto startOpTimeField =
            earliestOpenTransactionBson[SessionTxnRecord::kStartOpTimeFieldName];
        if (startOpTimeField.isABSONObj()) {
            startOpTime = OpTime::parse(startOpTimeField.Obj());
        }
    }
    return startOpTime;
}

SemiFuture<void> ShardMergeRecipientService::Instance::_getStartOpTimesFromDonor() {
    OpTime startApplyingDonorOpTime;

    stdx::unique_lock lk(_mutex);

    startApplyingDonorOpTime =
        OpTime(_getDonorBackupCursorInfo(lk).checkpointTimestamp, OpTime::kUninitializedTerm);

    // Unlock the mutex before doing network reads
    lk.unlock();

    auto oldestActiveTxnOpTime =
        _getOldestActiveTransactionAt(startApplyingDonorOpTime.getTimestamp());
    auto startFetchingDonorOpTime =
        oldestActiveTxnOpTime ? oldestActiveTxnOpTime : startApplyingDonorOpTime;

    pauseAfterRetrievingLastTxnMigrationRecipientInstance.pauseWhileSet();

    lk.lock();
    _stateDoc.setStartApplyingDonorOpTime(startApplyingDonorOpTime);
    _stateDoc.setStartFetchingDonorOpTime(startFetchingDonorOpTime);

    return _updateStateDocForMajority(lk);
}

void ShardMergeRecipientService::Instance::_processCommittedTransactionEntry(const BSONObj& entry) {
    auto sessionTxnRecord = SessionTxnRecord::parse(IDLParserContext("SessionTxnRecord"), entry);
    auto sessionId = sessionTxnRecord.getSessionId();
    auto txnNumber = sessionTxnRecord.getTxnNum();
    auto optTxnRetryCounter = sessionTxnRecord.getTxnRetryCounter();
    uassert(ErrorCodes::InvalidOptions,
            "txnRetryCounter is only supported in sharded clusters",
            !optTxnRetryCounter.has_value());

    auto uniqueOpCtx = cc().makeOperationContext();
    auto opCtx = uniqueOpCtx.get();

    // If the tenantMigrationInfo is set on the opCtx, we will set the
    // 'fromTenantMigration' field when writing oplog entries. That field is used to help recipient
    // secondaries determine if a no-op entry is related to a transaction entry.
    tenantMigrationInfo(opCtx) = boost::make_optional<TenantMigrationInfo>(getMigrationUUID());
    {
        auto lk = stdx::lock_guard(*opCtx->getClient());
        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNumber);
        opCtx->setInMultiDocumentTransaction();
    }
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

    LOGV2_DEBUG(7339737,
                1,
                "Migration attempting to commit transaction",
                "sessionId"_attr = sessionId,
                "txnNumber"_attr = txnNumber,
                "txnRetryCounter"_attr = optTxnRetryCounter,
                "migrationId"_attr = getMigrationUUID(),
                "entry"_attr = entry);

    auto txnParticipant = TransactionParticipant::get(opCtx);
    uassert(7339782,
            str::stream() << "Migration failed to get transaction participant for transaction "
                          << txnNumber << " on session " << sessionId,
            txnParticipant);

    // The in-memory transaction state may have been updated past the on-disk transaction state. For
    // instance, this might happen in an unprepared read-only transaction, which updates in-memory
    // but not on-disk. To prevent potential errors, we use the on-disk state for the following
    // transaction number checks.
    txnParticipant.invalidate(opCtx);
    txnParticipant.refreshFromStorageIfNeeded(opCtx);

    // If the entry's transaction number is stale/older than the current active transaction number
    // on the participant, fail the migration.
    uassert(ErrorCodes::TransactionTooOld,
            str::stream() << "Migration cannot apply transaction with tranaction number "
                          << txnNumber << " and transaction retry counter " << optTxnRetryCounter
                          << " on session " << sessionId
                          << " because a newer transaction with txnNumberAndRetryCounter: "
                          << txnParticipant.getActiveTxnNumberAndRetryCounter().toBSON()
                          << " has already started",
            txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber() <= txnNumber);
    if (txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber() == txnNumber) {
        // If the txn numbers are equal, move on to the next entry.
        return;
    }

    txnParticipant.beginOrContinueTransactionUnconditionally(opCtx,
                                                             {txnNumber, optTxnRetryCounter});

    MutableOplogEntry noopEntry;
    noopEntry.setOpType(repl::OpTypeEnum::kNoop);

    // Shard merge copies all tenants from the donor. This means that merge does
    // not need to filter prefetched committed transactions by tenantId. As a result, setting
    // a nss containing the tenantId for the no-op entry isn't necessary.
    noopEntry.setNss({});
    noopEntry.setObject({});

    noopEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());
    noopEntry.setSessionId(sessionId);
    noopEntry.setTxnNumber(txnNumber);
    noopEntry.getOperationSessionInfo().setTxnRetryCounter(optTxnRetryCounter);

    sessionTxnRecord.setStartOpTime(boost::none);
    // Use the same wallclock time as the noop entry.
    sessionTxnRecord.setLastWriteDate(noopEntry.getWallClockTime());

    AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
    writeConflictRetry(
        opCtx, "writeDonorCommittedTxnEntry", NamespaceString::kRsOplogNamespace.ns(), [&] {
            WriteUnitOfWork wuow(opCtx);

            // Write the no-op entry and update 'config.transactions'.
            auto opTime = repl::logOp(opCtx, &noopEntry);
            sessionTxnRecord.setLastWriteOpTime(std::move(opTime));
            TransactionParticipant::get(opCtx).onWriteOpCompletedOnPrimary(
                opCtx, {}, sessionTxnRecord);

            wuow.commit();
        });

    // Invalidate in-memory state so that the next time the session is checked out, it would reload
    // the transaction state from 'config.transactions'.
    txnParticipant.invalidate(opCtx);

    hangAfterUpdatingTransactionEntry.execute([&](const BSONObj& data) {
        LOGV2(7339707, "hangAfterUpdatingTransactionEntry failpoint enabled");
        hangAfterUpdatingTransactionEntry.pauseWhileSet();
        if (data["failAfterHanging"].trueValue()) {
            // Simulate the sync source shutting down/restarting.
            uasserted(ErrorCodes::ShutdownInProgress,
                      "Throwing error due to hangAfterUpdatingTransactionEntry failpoint");
        }
    });
}

SemiFuture<void>
ShardMergeRecipientService::Instance::_fetchCommittedTransactionsBeforeStartOpTime() {
    {
        auto opCtx = cc().makeOperationContext();
        _stopOrHangOnFailPoint(&fpBeforeFetchingCommittedTransactions, opCtx.get());
    }

    if (MONGO_unlikely(skipFetchingCommittedTransactions.shouldFail())) {  // Test-only.
        return SemiFuture<void>::makeReady();
    }

    {
        stdx::lock_guard lk(_mutex);
        if (_stateDoc.getCompletedUpdatingTransactionsBeforeStartOpTime()) {
            LOGV2_DEBUG(
                7339738,
                2,
                "Already completed fetching committed transactions from donor, skipping stage",
                "migrationId"_attr = getMigrationUUID());
            return SemiFuture<void>::makeReady();
        }
    }

    std::unique_ptr<DBClientCursor> cursor;
    cursor = _openCommittedTransactionsFindCursor();

    while (cursor->more()) {
        auto transactionEntry = cursor->next();
        _processCommittedTransactionEntry(transactionEntry);

        uassertStatusOK(_getInterruptStatus());
    }

    stdx::lock_guard lk(_mutex);
    _stateDoc.setCompletedUpdatingTransactionsBeforeStartOpTime(true);
    return _updateStateDocForMajority(lk);
}

std::unique_ptr<DBClientCursor>
ShardMergeRecipientService::Instance::_openCommittedTransactionsFindCursor() {
    Timestamp startApplyingDonorTimestamp;
    {
        stdx::lock_guard lk(_mutex);
        invariant(_stateDoc.getStartApplyingDonorOpTime());
        startApplyingDonorTimestamp = _stateDoc.getStartApplyingDonorOpTime()->getTimestamp();
    }

    FindCommandRequest findCommandRequest{NamespaceString::kSessionTransactionsTableNamespace};
    findCommandRequest.setFilter(BSON("state"
                                      << "committed"
                                      << "lastWriteOpTime.ts"
                                      << BSON("$lte" << startApplyingDonorTimestamp)));
    findCommandRequest.setReadConcern(
        ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    findCommandRequest.setHint(BSON("$natural" << 1));

    return _client->find(std::move(findCommandRequest), _readPreference, ExhaustMode::kOn);
}

void ShardMergeRecipientService::Instance::_createOplogBuffer(WithLock, OperationContext* opCtx) {
    OplogBufferCollection::Options options;
    options.peekCacheSize = static_cast<size_t>(tenantMigrationOplogBufferPeekCacheSize);
    options.dropCollectionAtStartup = false;
    options.dropCollectionAtShutdown = false;
    options.useTemporaryCollection = false;

    auto oplogBufferNS = getOplogBufferNs(getMigrationUUID());
    if (!_donorOplogBuffer) {

        auto bufferCollection = std::make_unique<OplogBufferCollection>(
            StorageInterface::get(opCtx), oplogBufferNS, options);
        _donorOplogBuffer = std::move(bufferCollection);
    }
}

SemiFuture<void>
ShardMergeRecipientService::Instance::_fetchRetryableWritesOplogBeforeStartOpTime() {
    _stopOrHangOnFailPoint(&fpAfterRetrievingStartOpTimesMigrationRecipientInstance);
    if (MONGO_unlikely(
            skipFetchingRetryableWritesEntriesBeforeStartOpTime.shouldFail())) {  // Test-only.
        return SemiFuture<void>::makeReady();
    }

    {
        stdx::lock_guard lk(_mutex);
        if (_stateDoc.getCompletedFetchingRetryableWritesBeforeStartOpTime()) {
            LOGV2_DEBUG(7339739,
                        2,
                        "Already completed fetching retryable writes oplog entries from donor, "
                        "skipping stage",
                        "migrationId"_attr = getMigrationUUID());
            return SemiFuture<void>::makeReady();
        }
    }

    auto opCtx = cc().makeOperationContext();
    auto expCtx = makeExpressionContext(opCtx.get());
    // If the oplog buffer contains entries at this point, it indicates that the recipient went
    // through failover before it finished writing all oplog entries to the buffer. Clear it and
    // redo the work.
    auto oplogBufferNS = getOplogBufferNs(getMigrationUUID());
    if (_donorOplogBuffer->getCount() > 0) {
        // Ensure we are primary when trying to clear the oplog buffer since it will drop and
        // re-create the collection.
        auto coordinator = repl::ReplicationCoordinator::get(opCtx.get());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);
        if (!coordinator->canAcceptWritesForDatabase(opCtx.get(), oplogBufferNS.db())) {
            uassertStatusOK(
                Status(ErrorCodes::NotWritablePrimary,
                       "Recipient node is not primary, cannot clear oplog buffer collection."));
        }
        _donorOplogBuffer->clear(opCtx.get());
    }

    Timestamp startFetchingTimestamp;
    {
        stdx::lock_guard lk(_mutex);
        invariant(_stateDoc.getStartFetchingDonorOpTime());
        startFetchingTimestamp = _stateDoc.getStartFetchingDonorOpTime().value().getTimestamp();
    }

    LOGV2_DEBUG(7339740,
                1,
                "Pre-fetching retryable oplog entries before startFetchingTimstamp",
                "startFetchingTimestamp"_attr = startFetchingTimestamp,
                "migrationId"_attr = getMigrationUUID());

    // Fetch the oplog chains of all retryable writes that occurred before startFetchingTimestamp.
    std::vector<BSONObj> serializedPipeline;
    serializedPipeline =
        tenant_migration_util::createRetryableWritesOplogFetchingPipelineForAllTenants(
            expCtx, startFetchingTimestamp)
            ->serializeToBson();


    AggregateCommandRequest aggRequest(NamespaceString::kSessionTransactionsTableNamespace,
                                       std::move(serializedPipeline));

    // Use local read concern. This is because secondary oplog application coalesces multiple
    // updates to the same config.transactions record into a single update of the most recent
    // retryable write statement, and since after SERVER-47844, the committed snapshot of a
    // secondary can be in the middle of batch, the combination of these two makes secondary
    // majority reads on config.transactions not always reflect committed retryable writes at
    // that majority commit point. So we need to do a local read to fetch the retryable writes
    // so that we don't miss the config.transactions record and later do a majority read on the
    // donor's last applied operationTime to make sure the fetched results are majority committed.
    auto readConcernArgs = repl::ReadConcernArgs(
        boost::optional<repl::ReadConcernLevel>(repl::ReadConcernLevel::kLocalReadConcern));
    aggRequest.setReadConcern(readConcernArgs.toBSONInner());
    // We must set a writeConcern on internal commands.
    aggRequest.setWriteConcern(WriteConcernOptions());
    // Allow aggregation to write to temporary files in case it reaches memory restriction.
    aggRequest.setAllowDiskUse(true);

    // Failpoint to set a small batch size on the aggregation request.
    if (MONGO_unlikely(fpSetSmallAggregationBatchSize.shouldFail())) {
        SimpleCursorOptions cursor;
        cursor.setBatchSize(1);
        aggRequest.setCursor(cursor);
    }

    std::unique_ptr<DBClientCursor> cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        _client.get(), std::move(aggRequest), true /* secondaryOk */, false /* useExhaust */));

    // cursor->more() will automatically request more from the server if necessary.
    while (cursor->more()) {
        // Similar to the OplogFetcher, we keep track of each oplog entry to apply and the number of
        // the bytes of the documents read off the network.
        std::vector<BSONObj> retryableWritesEntries;
        retryableWritesEntries.reserve(cursor->objsLeftInBatch());
        auto toApplyDocumentBytes = 0;

        while (cursor->moreInCurrentBatch()) {
            // Gather entries from current batch.
            BSONObj doc = cursor->next();
            toApplyDocumentBytes += doc.objsize();
            retryableWritesEntries.push_back(doc);
        }

        if (retryableWritesEntries.size() != 0) {
            // Wait for enough space.
            _donorOplogBuffer->waitForSpace(opCtx.get(), toApplyDocumentBytes);
            // Buffer retryable writes entries.
            _donorOplogBuffer->preload(
                opCtx.get(), retryableWritesEntries.begin(), retryableWritesEntries.end());
        }

        pauseAfterRetrievingRetryableWritesBatch.pauseWhileSet();

        // In between batches, check for recipient failover.
        uassertStatusOK(_getInterruptStatus());
    }

    // Do a majority read on the sync source to make sure the pre-fetch result exists on a
    // majority of nodes in the set. The timestamp we wait on is the donor's last applied
    // operationTime, which is guaranteed to be at batch boundary if the sync source is a
    // secondary. We do not check the rollbackId - rollback would lead to the sync source
    // closing connections so the migration would fail and retry.
    auto operationTime = cursor->getOperationTime();
    uassert(7339783,
            "Donor operationTime not available in retryable write pre-fetch result.",
            operationTime);
    LOGV2_DEBUG(7339741,
                1,
                "Waiting for retryable write pre-fetch result to be majority committed.",
                "operationTime"_attr = operationTime);

    fpBeforeWaitingForRetryableWritePreFetchMajorityCommitted.pauseWhileSet();

    BSONObj readResult;
    BSONObj cmd = ClonerUtils::buildMajorityWaitRequest(*operationTime);
    _client.get()->runCommand(
        DatabaseName(boost::none, "admin"), cmd, readResult, QueryOption_SecondaryOk);
    uassertStatusOKWithContext(
        getStatusFromCommandResult(readResult),
        "Failed to wait for retryable writes pre-fetch result majority committed");

    // Update _stateDoc to indicate that we've finished the retryable writes oplog entry fetching
    // stage.
    stdx::lock_guard lk(_mutex);
    _stateDoc.setCompletedFetchingRetryableWritesBeforeStartOpTime(true);
    return _updateStateDocForMajority(lk);
}

void ShardMergeRecipientService::Instance::_startOplogBuffer(OperationContext* opCtx) try {
    // It is illegal to start the replicated donor buffer when the node is not primary.
    // So ensure we are primary before trying to startup the oplog buffer.
    repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);

    auto oplogBufferNS = getOplogBufferNs(getMigrationUUID());
    if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx,
                                                                              oplogBufferNS.db())) {
        uassertStatusOK(
            Status(ErrorCodes::NotWritablePrimary, "Recipient node is no longer a primary."));
    }

    _donorOplogBuffer->startup(opCtx);

} catch (DBException& ex) {
    ex.addContext("Failed to create oplog buffer collection.");
    throw;
}

void ShardMergeRecipientService::Instance::_startOplogFetcher() {
    _stopOrHangOnFailPoint(&fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime);

    auto opCtx = cc().makeOperationContext();
    // Start the oplog buffer outside the mutex to avoid deadlock on a concurrent stepdown.
    _startOplogBuffer(opCtx.get());

    const auto donorMajorityOpTime = _getDonorMajorityOpTime(_oplogFetcherClient);

    stdx::lock_guard lk(_mutex);

    auto startFetchOpTime = _stateDoc.getStartFetchingDonorOpTime();
    invariant(startFetchOpTime && !startFetchOpTime->isNull());

    if (donorMajorityOpTime < *startFetchOpTime) {
        LOGV2_ERROR(7339742,
                    "Donor sync source's majority OpTime is behind our startFetchOpTime",
                    "migrationId"_attr = getMigrationUUID(),
                    "donorMajorityOpTime"_attr = donorMajorityOpTime,
                    "startFetchOpTime"_attr = *startFetchOpTime);
        const auto now = _serviceContext->getFastClockSource()->now();
        _excludeDonorHost(lk,
                          _oplogFetcherClient->getServerHostAndPort(),
                          now + Milliseconds(tenantMigrationExcludeDonorHostTimeoutMS));
        uasserted(ErrorCodes::InvalidSyncSource,
                  "Donor sync source's majority OpTime is behind our startFetchOpTime, retrying "
                  "sync source selection");
    }

    OplogFetcher::Config oplogFetcherConfig(
        *startFetchOpTime,
        _oplogFetcherClient->getServerHostAndPort(),
        // The config is only used for setting the awaitData timeout; the defaults are fine.
        ReplSetConfig::parse(BSON("_id"
                                  << "dummy"
                                  << "version" << 1 << "members" << BSONArray(BSONObj()))),
        // We do not need to check the rollback ID.
        ReplicationProcess::kUninitializedRollbackId,
        tenantMigrationOplogFetcherBatchSize,
        OplogFetcher::RequireFresherSyncSource::kDontRequireFresherSyncSource,
        true /* forTenantMigration */);

    oplogFetcherConfig.queryReadConcern =
        ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);
    oplogFetcherConfig.name = "TenantOplogFetcher_" + getMigrationUUID().toString();
    oplogFetcherConfig.startingPoint = OplogFetcher::StartingPoint::kEnqueueFirstDoc;

    _dataReplicatorExternalState = std::make_unique<DataReplicatorExternalStateTenantMigration>();

    // Starting oplog fetcher after migration interrupt would cause the fetcher to fail
    // due to closed _oplogFetcherClient connection.
    _donorOplogFetcher = (*_createOplogFetcherFn)(
        (**_scopedExecutor).get(),
        std::make_unique<OplogFetcherRestartDecisionTenantMigration>(),
        _dataReplicatorExternalState.get(),
        [this, self = shared_from_this()](OplogFetcher::Documents::const_iterator first,
                                          OplogFetcher::Documents::const_iterator last,
                                          const OplogFetcher::DocumentsInfo& info) {
            return _enqueueDocuments(first, last, info);
        },
        [this, self = shared_from_this()](const Status& s, int rbid) { _oplogFetcherCallback(s); },
        std::move(oplogFetcherConfig));
    _donorOplogFetcher->setConnection(std::move(_oplogFetcherClient));
    uassertStatusOK(_donorOplogFetcher->startup());
}

Status ShardMergeRecipientService::Instance::_enqueueDocuments(
    OplogFetcher::Documents::const_iterator begin,
    OplogFetcher::Documents::const_iterator end,
    const OplogFetcher::DocumentsInfo& info) {

    invariant(_donorOplogBuffer);

    if (info.toApplyDocumentCount != 0) {
        auto opCtx = cc().makeOperationContext();
        // Wait for enough space.
        _donorOplogBuffer->waitForSpace(opCtx.get(), info.toApplyDocumentBytes);
        // Buffer docs for later application.
        _donorOplogBuffer->push(opCtx.get(), begin, end);
    }

    return Status::OK();
}

void ShardMergeRecipientService::Instance::_oplogFetcherCallback(Status oplogFetcherStatus) {
    // The oplog fetcher is normally canceled when migration is done; any other error
    // indicates failure.
    if (oplogFetcherStatus.isOK()) {
        // Oplog fetcher status of "OK" means the stopReplProducer failpoint is set.  Migration
        // cannot continue in this state so force a failure.
        LOGV2_ERROR(
            7339743,
            "Recipient migration instance oplog fetcher stopped due to stopReplProducer failpoint",
            "migrationId"_attr = getMigrationUUID());
        interruptConditionally(
            {ErrorCodes::Error(7339793),
             "Recipient migration instance oplog fetcher stopped due to stopReplProducer "
             "failpoint"});
    } else if (oplogFetcherStatus.code() != ErrorCodes::CallbackCanceled) {
        LOGV2_ERROR(7339744,
                    "Recipient migration instance oplog fetcher failed",
                    "migrationId"_attr = getMigrationUUID(),
                    "error"_attr = oplogFetcherStatus);
        if (isRetriableOplogFetcherError(oplogFetcherStatus)) {
            LOGV2_DEBUG(7339745,
                        1,
                        "Recipient migration instance oplog fetcher received retriable error, "
                        "excluding donor host as sync source and retrying",
                        "migrationId"_attr = getMigrationUUID(),
                        "error"_attr = oplogFetcherStatus);

            stdx::lock_guard lk(_mutex);
            const auto now = _serviceContext->getFastClockSource()->now();
            _excludeDonorHost(lk,
                              _client->getServerHostAndPort(),
                              now + Milliseconds(tenantMigrationExcludeDonorHostTimeoutMS));
        }
        interruptConditionally(oplogFetcherStatus);
    }
}

void ShardMergeRecipientService::Instance::_stopOrHangOnFailPoint(FailPoint* fp,
                                                                  OperationContext* opCtx) {
    auto shouldHang = false;
    fp->executeIf(
        [&](const BSONObj& data) {
            LOGV2(7339708,
                  "Shard merge recipient instance: failpoint enabled",
                  "migrationId"_attr = getMigrationUUID(),
                  "name"_attr = fp->getName(),
                  "args"_attr = data);
            if (data["action"].str() == "hang") {
                // fp is locked. If we call pauseWhileSet here, another thread can't disable fp.
                shouldHang = true;
            } else {
                uasserted(data["stopErrorCode"].numberInt(),
                          "Skipping remaining processing due to fail point");
            }
        },
        [&](const BSONObj& data) {
            auto action = data["action"].str();
            return (action == "hang" || action == "stop");
        });

    if (shouldHang) {
        if (opCtx) {
            fp->pauseWhileSet(opCtx);
        } else {
            fp->pauseWhileSet();
        }
    }
}

SemiFuture<void>
ShardMergeRecipientService::Instance::_advanceMajorityCommitTsToBkpCursorCheckpointTs(
    const CancellationToken& token) {
    auto uniqueOpCtx = cc().makeOperationContext();
    auto opCtx = uniqueOpCtx.get();

    Timestamp donorBkpCursorCkptTs;
    {
        stdx::lock_guard lk(_mutex);
        donorBkpCursorCkptTs = _getDonorBackupCursorInfo(lk).checkpointTimestamp;
    }

    if (opCtx->getServiceContext()->getStorageEngine()->getStableTimestamp() >=
        donorBkpCursorCkptTs) {
        return SemiFuture<void>::makeReady();
    }

    LOGV2(
        7339709,
        "Advancing recipient's majority commit timestamp to be at least the donor's backup cursor "
        "checkpoint timestamp",
        "migrationId"_attr = getMigrationUUID(),
        "donorBackupCursorCheckpointTimestamp"_attr = donorBkpCursorCkptTs);

    _stopOrHangOnFailPoint(&fpBeforeAdvancingStableTimestamp, opCtx);

    // Advance the cluster time to the donorBkpCursorCkptTs so that we ensure we
    // write the no-op entry below at ts > donorBkpCursorCkptTs.
    VectorClockMutable::get(_serviceContext)->tickClusterTimeTo(LogicalTime(donorBkpCursorCkptTs));

    writeConflictRetry(opCtx,
                       "mergeRecipientWriteNoopToAdvanceStableTimestamp",
                       NamespaceString::kRsOplogNamespace.ns(),
                       [&] {
                           if (token.isCanceled()) {
                               return;
                           }
                           AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
                           WriteUnitOfWork wuow(opCtx);
                           const std::string msg = str::stream()
                               << "Merge recipient advancing stable timestamp";
                           opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                               opCtx, BSON("msg" << msg));
                           wuow.commit();
                       });

    // Get the timestamp of the no-op. This will have ts > donorBkpCursorCkptTs.
    auto noOpTs = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    return WaitForMajorityService::get(opCtx->getServiceContext()).waitUntilMajority(noOpTs, token);
}

SemiFuture<void> ShardMergeRecipientService::Instance::_durablyPersistConsistentState() {
    _stopOrHangOnFailPoint(&fpBeforeMarkingCloneSuccess);

    stdx::lock_guard lk(_mutex);
    _stateDoc.setCloneFinishedRecipientOpTime(
        repl::ReplicationCoordinator::get(cc().getServiceContext())->getMyLastAppliedOpTime());
    // Mark the migration has reached consistent state.
    _stateDoc.setState(ShardMergeRecipientStateEnum::kConsistent);
    return _updateStateDocForMajority(lk);
}

SemiFuture<void> ShardMergeRecipientService::Instance::_enterConsistentState() {
    return _durablyPersistConsistentState()
        .thenRunOn(**_scopedExecutor)
        .then([this, self = shared_from_this()]() {
            _stopOrHangOnFailPoint(&fpBeforeFulfillingDataConsistentPromise);
            stdx::lock_guard lk(_mutex);

            auto donorConsistentOpTime = _stateDoc.getStartApplyingDonorOpTime();
            invariant(donorConsistentOpTime && !donorConsistentOpTime->isNull());

            LOGV2_DEBUG(7339746,
                        1,
                        "Recipient migration instance is in consistent state",
                        "migrationId"_attr = getMigrationUUID(),
                        "donorConsistentOpTime"_attr = *donorConsistentOpTime);
            setPromiseValueIfNotReady(lk, _dataConsistentPromise, *donorConsistentOpTime);
        })
        .semi();
}

void ShardMergeRecipientService::Instance::onMemberImportedFiles(
    const HostAndPort& host, bool success, const boost::optional<StringData>& reason) {
    stdx::lock_guard lk(_mutex);
    if (!_waitingForMembersToImportFiles) {
        LOGV2_WARNING(7339747,
                      "Ignoring delayed recipientVoteImportedFiles",
                      "host"_attr = host.toString(),
                      "migrationId"_attr = _migrationUuid);
        return;
    }

    auto state = _stateDoc.getState();
    uassert(7339785,
            "The migration is at the wrong stage for recipientVoteImportedFiles: {}"_format(
                ShardMergeRecipientState_serializer(state)),
            state == ShardMergeRecipientStateEnum::kLearnedFilenames);

    if (!success) {
        _importedFilesPromise.setError(
            {ErrorCodes::OperationFailed,
             "Migration failed on {}, error: {}"_format(host, reason.value_or("null"))});
        _waitingForMembersToImportFiles = false;
        return;
    }

    _membersWhoHaveImportedFiles.insert(host);
    // Not reconfig-safe, we must not do a reconfig concurrent with a migration.
    if (static_cast<int>(_membersWhoHaveImportedFiles.size()) ==
        repl::ReplicationCoordinator::get(_serviceContext)
            ->getConfig()
            .getNumDataBearingMembers()) {
        LOGV2_INFO(7339748,
                   "All members finished importing donated files",
                   "migrationId"_attr = _migrationUuid);
        _importedFilesPromise.emplaceValue();
        _waitingForMembersToImportFiles = false;
    }
}

SemiFuture<void> ShardMergeRecipientService::Instance::_markStateDocAsGarbageCollectable() {
    _stopOrHangOnFailPoint(&fpBeforeMarkingStateDocAsGarbageCollectable);

    stdx::lock_guard lk(_mutex);
    if (_stateDoc.getExpireAt()) {
        // Nothing to do if the state doc already has the expireAt set.
        return SemiFuture<void>::makeReady();
    }

    _stateDoc.setExpireAt(_serviceContext->getFastClockSource()->now() +
                          Milliseconds{repl::tenantMigrationGarbageCollectionDelayMS.load()});

    return _updateStateDocForMajority(lk);
}

Status ShardMergeRecipientService::Instance::_getInterruptStatus() const {
    if (auto future = _interruptPromise.getFuture(); future.isReady()) {
        return future.getNoThrow();
    }
    return Status::OK();
}

void ShardMergeRecipientService::Instance::_cancelRemainingWork(WithLock lk, Status status) {
    setPromiseErrorifNotReady(lk, _interruptPromise, status);

    if (_client) {
        _client->shutdownAndDisallowReconnect();
    }

    if (_oplogFetcherClient) {
        // Closing this connection will be cause tenant oplog fetcher to fail.
        _oplogFetcherClient->shutdownAndDisallowReconnect();
    }

    shutdownTarget(lk, _donorFilenameBackupCursorFileFetcher);
    shutdownTarget(lk, _tenantOplogApplier);
}

void ShardMergeRecipientService::Instance::interrupt(Status status) {
    stdx::lock_guard lk(_mutex);
    setPromiseErrorifNotReady(lk, _receivedRecipientForgetMigrationPromise, status);
    _cancelRemainingWork(lk, status);
}

void ShardMergeRecipientService::Instance::interruptConditionally(Status status) {
    stdx::lock_guard lk(_mutex);
    _cancelRemainingWork(lk, status);
}

void ShardMergeRecipientService::Instance::onReceiveRecipientForgetMigration(
    OperationContext* opCtx, const MigrationDecisionEnum& decision) {
    LOGV2(7339710,
          "Forgetting migration due to recipientForgetMigration command",
          "migrationId"_attr = getMigrationUUID());

    stdx::lock_guard lk(_mutex);
    setPromiseValueIfNotReady(lk, _receivedRecipientForgetMigrationPromise, decision);
    _cancelRemainingWork(lk,
                         Status(ErrorCodes::TenantMigrationForgotten,
                                str::stream() << "recipientForgetMigration received for migration "
                                              << getMigrationUUID()));
}

void ShardMergeRecipientService::Instance::_cleanupOnMigrationCompletion(Status status) {
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<OplogFetcher> savedDonorOplogFetcher;
    std::shared_ptr<TenantOplogApplier> savedTenantOplogApplier;
    std::unique_ptr<ThreadPool> savedWriterPool;
    std::unique_ptr<Fetcher> savedDonorFilenameBackupCursorFileFetcher;
    {
        stdx::lock_guard lk(_mutex);
        _cancelRemainingWork(lk, status);

        _backupCursorKeepAliveCancellation = {};
        _backupCursorKeepAliveFuture = boost::none;

        shutdownTarget(lk, _donorOplogFetcher);
        shutdownTargetWithOpCtx(lk, _donorOplogBuffer, opCtx.get());

        _donorReplicaSetMonitor = nullptr;

        invariant(!status.isOK());
        setPromiseErrorifNotReady(lk, _dataConsistentPromise, status);
        setPromiseErrorifNotReady(lk, _migrationCompletionPromise, status);

        _oplogApplierReady = false;
        _oplogApplierReadyCondVar.notify_all();

        // Save them to join() with it outside of _mutex.
        using std::swap;
        swap(savedDonorOplogFetcher, _donorOplogFetcher);
        swap(savedTenantOplogApplier, _tenantOplogApplier);
        swap(savedWriterPool, _writerPool);
        swap(savedDonorFilenameBackupCursorFileFetcher, _donorFilenameBackupCursorFileFetcher);
    }

    // Perform join outside the lock to avoid deadlocks.
    if (savedDonorFilenameBackupCursorFileFetcher) {
        invariantStatusOK(
            savedDonorFilenameBackupCursorFileFetcher->join(Interruptible::notInterruptible()));
    }
    joinTarget(savedDonorOplogFetcher);
    joinTarget(savedTenantOplogApplier);
    if (savedWriterPool) {
        savedWriterPool->shutdown();
        savedWriterPool->join();
    }
}

SemiFuture<void> ShardMergeRecipientService::Instance::_updateStateDocForMajority(
    WithLock lk, const RegisterChangeCbk& registerChange) {
    return _writeStateDocForMajority(lk, OpType::kUpdate, registerChange);
}

SemiFuture<void> ShardMergeRecipientService::Instance::_insertStateDocForMajority(
    WithLock lk, const RegisterChangeCbk& registerChange) {
    return _writeStateDocForMajority(lk, OpType::kInsert, registerChange);
}

SemiFuture<void> ShardMergeRecipientService::Instance::_writeStateDocForMajority(
    WithLock, OpType opType, const RegisterChangeCbk& registerChange) {
    return ExecutorFuture(**_scopedExecutor)
        .then([this, self = shared_from_this(), stateDoc = _stateDoc, opType, registerChange] {
            auto opCtx = cc().makeOperationContext();
            _writeStateDoc(opCtx.get(), stateDoc, opType, registerChange);
            auto writeOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            return WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(writeOpTime, CancellationToken::uncancelable());
        })
        .semi();
}

void ShardMergeRecipientService::Instance::_writeStateDoc(
    OperationContext* opCtx,
    const ShardMergeRecipientDocument& stateDoc,
    OpType opType,
    const RegisterChangeCbk& registerChange) {
    const auto& nss = NamespaceString::kShardMergeRecipientsNamespace;
    AutoGetCollection collection(opCtx, nss, MODE_IX);

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << nss.toStringForErrorMsg() << " does not exist",
            collection);

    writeConflictRetry(opCtx, "writeShardMergeRecipientStateDoc", nss.ns(), [&]() {
        WriteUnitOfWork wunit(opCtx);

        if (registerChange)
            registerChange(opCtx);

        const auto filter =
            BSON(TenantMigrationRecipientDocument::kIdFieldName << stateDoc.getId());
        auto updateResult = Helpers::upsert(opCtx,
                                            nss,
                                            filter,
                                            stateDoc.toBSON(),
                                            /*fromMigrate=*/false);
        switch (opType) {
            case OpType::kInsert:
                uassert(ErrorCodes::ConflictingOperationInProgress,
                        str::stream()
                            << "Failed to insert the shard merge recipient state doc: "
                            << tenant_migration_util::redactStateDoc(stateDoc.toBSON())
                            << "; Found active migration for migrationId: " << stateDoc.getId(),
                        !updateResult.upsertedId.isEmpty());
                break;
            case OpType::kUpdate:
                // Intentionally not checking `updateResult.numDocsModified` to handle no-op
                // updates.
                uassert(ErrorCodes::NoSuchKey,
                        str::stream()
                            << "Failed to update shard merge recipient state document due to "
                               "missing state document for migrationId: "
                            << stateDoc.getId(),
                        updateResult.numMatched);
                break;
            default:
                MONGO_UNREACHABLE
        }

        wunit.commit();
    });
}

void ShardMergeRecipientService::Instance::_assertIfMigrationIsSafeToRunWithCurrentFcv() {
    //(Generic FCV reference): This FCV check should exist across LTS binary versions.
    auto recipientFCV = serverGlobalParams.featureCompatibility.getVersion();
    if (serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading(recipientFCV)) {
        LOGV2(7339711, "Must abort shard merge as recipient is upgrading or downgrading");
        uasserted(ErrorCodes::TenantMigrationAborted,
                  "Can't run shard merge when FCV is downgrading or upgrading");
    }

    _stopOrHangOnFailPoint(&fpAfterRecordingRecipientPrimaryStartingFCV);
    if (skipComparingRecipientAndDonorFCV.shouldFail()) {  // Test-only.
        return;
    }

    FindCommandRequest findCmd{NamespaceString::kServerConfigurationNamespace};
    findCmd.setFilter(BSON("_id" << multiversion::kParameterName));
    findCmd.setReadConcern(ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    auto donorFCVbson = _client->findOne(std::move(findCmd),
                                         ReadPreferenceSetting{ReadPreference::SecondaryPreferred});

    uassert(7339755, "FCV on donor not set", !donorFCVbson.isEmpty());

    auto swDonorFCV = FeatureCompatibilityVersionParser::parse(donorFCVbson);
    uassertStatusOK(swDonorFCV.getStatus());

    auto donorFCV = swDonorFCV.getValue();
    if (donorFCV != recipientFCV) {
        LOGV2_ERROR(7339749,
                    "Donor and recipient FCV mismatch",
                    "migrationId"_attr = getMigrationUUID(),
                    "donorConnString"_attr = _donorConnectionString,
                    "donorFCV"_attr = donorFCV,
                    "recipientFCV"_attr = recipientFCV);
        uasserted(7339756, "Mismatch between donor and recipient FCVs");
    }

    _stopOrHangOnFailPoint(&fpAfterComparingRecipientAndDonorFCV);
}

void ShardMergeRecipientService::Instance::_startOplogApplier() {
    _stopOrHangOnFailPoint(&fpAfterFetchingCommittedTransactions);

    stdx::unique_lock lk(_mutex);
    // Don't start the tenant oplog applier if the migration is interrupted.
    uassertStatusOK(_getInterruptStatus());

    const auto& startApplyingDonorOpTime = _stateDoc.getStartApplyingDonorOpTime();
    invariant(startApplyingDonorOpTime);
    const auto& cloneFinishedRecipientOpTime = _stateDoc.getCloneFinishedRecipientOpTime();
    invariant(cloneFinishedRecipientOpTime);

    _tenantOplogApplier = std::make_shared<TenantOplogApplier>(_migrationUuid,
                                                               MigrationProtocolEnum::kShardMerge,
                                                               boost::none,
                                                               *startApplyingDonorOpTime,
                                                               _donorOplogBuffer.get(),
                                                               **_scopedExecutor,
                                                               _writerPool.get());
    _tenantOplogApplier->setCloneFinishedRecipientOpTime(*cloneFinishedRecipientOpTime);

    LOGV2_DEBUG(7339750,
                1,
                "Recipient migration instance starting oplog applier",
                "migrationId"_attr = getMigrationUUID(),
                "startApplyingAfterDonorOpTime"_attr =
                    _tenantOplogApplier->getStartApplyingAfterOpTime());

    uassertStatusOK(_tenantOplogApplier->startup());
    _oplogApplierReady = true;
    _oplogApplierReadyCondVar.notify_all();

    lk.unlock();
    _stopOrHangOnFailPoint(&fpAfterStartingOplogApplierMigrationRecipientInstance);
}

void ShardMergeRecipientService::Instance::_setup(ConnectionPair connectionPair) {
    auto uniqueOpCtx = cc().makeOperationContext();
    auto opCtx = uniqueOpCtx.get();

    stdx::lock_guard lk(_mutex);
    // Do not set the internal states if the migration is already interrupted.
    uassertStatusOK(_getInterruptStatus());

    _client = std::move(connectionPair.first);
    _oplogFetcherClient = std::move(connectionPair.second);

    _writerPool = makeTenantMigrationWriterPool();

    _sharedData = std::make_unique<TenantMigrationSharedData>(_serviceContext->getFastClockSource(),
                                                              getMigrationUUID());

    _createOplogBuffer(lk, opCtx);
}

void ShardMergeRecipientService::Instance::_fetchAndStoreDonorClusterTimeKeyDocs(
    const CancellationToken& token) {
    std::vector<ExternalKeysCollectionDocument> keyDocs;
    FindCommandRequest findRequest{NamespaceString::kKeysCollectionNamespace};
    auto cursor = _client->find(std::move(findRequest), _readPreference);
    while (cursor->more()) {
        const auto doc = cursor->nextSafe().getOwned();
        keyDocs.push_back(
            tenant_migration_util::makeExternalClusterTimeKeyDoc(_migrationUuid, doc));
    }

    tenant_migration_util::storeExternalClusterTimeKeyDocs(std::move(keyDocs));
}

bool ShardMergeRecipientService::Instance::_isCommitOrAbortState(WithLock) const {
    auto state = _stateDoc.getState();
    return state == ShardMergeRecipientStateEnum::kAborted ||
        state == ShardMergeRecipientStateEnum::kCommitted;
}

SemiFuture<void> ShardMergeRecipientService::Instance::_prepareForMigration(
    const CancellationToken& token) {
    _stopOrHangOnFailPoint(&fpAfterPersistingTenantMigrationRecipientInstanceStateDoc);

    return _createAndConnectClients()
        .thenRunOn(**_scopedExecutor)
        .then([this, self = shared_from_this(), token](ConnectionPair connectionPair) {
            _stopOrHangOnFailPoint(&fpAfterConnectingTenantMigrationRecipientInstance);
            _setup(std::move(connectionPair));

            _stopOrHangOnFailPoint(&fpBeforeFetchingDonorClusterTimeKeys);
            _fetchAndStoreDonorClusterTimeKeyDocs(token);
        })
        .semi();
}

SemiFuture<void> ShardMergeRecipientService::Instance::_waitForAllNodesToFinishImport() {
    _stopOrHangOnFailPoint(&fpAfterStartingOplogFetcherMigrationRecipientInstance);
    LOGV2_INFO(7339751, "Waiting for all nodes to call recipientVoteImportedFiles");
    return _importedFilesPromise.getFuture().semi();
}

SemiFuture<void> ShardMergeRecipientService::Instance::_startMigrationIfSafeToRunwithCurrentFCV(
    const CancellationToken& token) {
    _assertIfMigrationIsSafeToRunWithCurrentFcv();
    return _openBackupCursorWithRetry(token)
        .thenRunOn(**_scopedExecutor)
        .then([this, self = shared_from_this(), token] { _keepBackupCursorAlive(token); })
        .then([this, self = shared_from_this(), token] {
            return _advanceMajorityCommitTsToBkpCursorCheckpointTs(token);
        })
        .then([this, self = shared_from_this()] { return _enterLearnedFilenamesState(); })
        .then([this, self = shared_from_this()]() { return _getStartOpTimesFromDonor(); })
        .then([this, self = shared_from_this()] {
            return _fetchRetryableWritesOplogBeforeStartOpTime();
        })
        .then([this, self = shared_from_this()] { _startOplogFetcher(); })
        .then([this, self = shared_from_this()] { return _waitForAllNodesToFinishImport(); })
        .then([this, self = shared_from_this()] { return _killBackupCursor(); })
        .then([this, self = shared_from_this()] { return _enterConsistentState(); })
        .then([this, self = shared_from_this()] {
            return _fetchCommittedTransactionsBeforeStartOpTime();
        })
        .then([this, self = shared_from_this()] { return _startOplogApplier(); })
        .semi();
}

SemiFuture<TenantOplogApplier::OpTimePair>
ShardMergeRecipientService::Instance::_waitForMigrationToComplete() {
    _stopOrHangOnFailPoint(&fpAfterDataConsistentMigrationRecipientInstance);

    stdx::lock_guard lk(_mutex);
    // wait for oplog applier to complete/stop.
    // The oplog applier does not exit normally; it must be shut down externally,
    // e.g. by recipientForgetMigration.
    return _tenantOplogApplier->getNotificationForOpTime(OpTime::max());
}

void ShardMergeRecipientService::Instance::_dropTempCollections() {
    _stopOrHangOnFailPoint(&fpBeforeDroppingTempCollections);

    auto opCtx = cc().makeOperationContext();
    auto storageInterface = StorageInterface::get(opCtx.get());

    // The donated files and oplog buffer collections can be safely dropped at this
    // point. In case either collection does not exist, dropping will be a no-op.
    // It isn't necessary that a given drop is majority-committed. A new primary will
    // attempt to drop the collection anyway.
    uassertStatusOK(storageInterface->dropCollection(
        opCtx.get(), shard_merge_utils::getDonatedFilesNs(getMigrationUUID())));

    uassertStatusOK(
        storageInterface->dropCollection(opCtx.get(), getOplogBufferNs(getMigrationUUID())));
}

SemiFuture<void> ShardMergeRecipientService::Instance::_durablyPersistCommitAbortDecision(
    MigrationDecisionEnum decision) {
    _stopOrHangOnFailPoint(&fpAfterReceivingRecipientForgetMigration);
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (_isCommitOrAbortState(lk)) {
            return SemiFuture<void>::makeReady();
        }
    }

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    const auto& nss = NamespaceString::kShardMergeRecipientsNamespace;

    AutoGetCollection collection(opCtx, nss, MODE_IX);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << nss.toStringForErrorMsg() << " does not exist",
            collection);

    writeConflictRetry(opCtx, "markShardMergeStateDocGarbageCollectable", nss.ns(), [&]() {
        WriteUnitOfWork wuow(opCtx);
        auto oplogSlot = LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];
        const auto originalRecordId =
            Helpers::findById(opCtx, collection.getCollection(), BSON("_id" << _migrationUuid));

        auto stateDoc = [&]() {
            stdx::lock_guard<Latch> lg(_mutex);
            switch (decision) {
                case MigrationDecisionEnum::kCommitted:
                    LOGV2_DEBUG(7339760,
                                2,
                                "Marking recipient migration instance as committed ",
                                "migrationId"_attr = _migrationUuid);
                    _stateDoc.setState(ShardMergeRecipientStateEnum::kCommitted);
                    break;
                case MigrationDecisionEnum::kAborted:
                    LOGV2_DEBUG(7339791,
                                2,
                                "Marking recipient migration instance as aborted ",
                                "migrationId"_attr = _migrationUuid,
                                "abortOpTime"_attr = oplogSlot);
                    _stateDoc.setState(ShardMergeRecipientStateEnum::kAborted);
                    _stateDoc.setAbortOpTime(oplogSlot);
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
            if (originalRecordId.isNull()) {
                // It's possible to get here only for following cases.
                // 1) The migration was forgotten before receiving a 'recipientSyncData'.
                // 2) A delayed 'recipientForgetMigration' was received after the state doc was
                // deleted.
                // 3) Fail to initialize the state document.
                invariant(_stateDoc.getStartGarbageCollect());
                _stateDoc.setStartAt(_serviceContext->getFastClockSource()->now());
            }
            return _stateDoc.toBSON();
        }();

        if (originalRecordId.isNull()) {
            uassertStatusOK(collection_internal::insertDocument(
                opCtx,
                *collection,
                InsertStatement(kUninitializedStmtId, stateDoc, oplogSlot),
                nullptr));

        } else {
            auto preImageDoc = collection->docFor(opCtx, originalRecordId);
            CollectionUpdateArgs args{preImageDoc.value()};
            args.criteria = BSON("_id" << _migrationUuid);
            args.oplogSlots = {oplogSlot};
            args.update = stateDoc;

            collection_internal::updateDocument(opCtx,
                                                *collection,
                                                originalRecordId,
                                                preImageDoc,
                                                stateDoc,
                                                collection_internal::kUpdateAllIndexes,
                                                nullptr /* indexesAffected */,
                                                nullptr /* OpDebug* */,
                                                &args);
        }

        wuow.commit();
    });

    auto waitOptime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    return WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(waitOptime, CancellationToken::uncancelable());
}

SemiFuture<void>
ShardMergeRecipientService::Instance::_waitForForgetMigrationThenMarkMigrationGarbageCollectable(
    const CancellationToken& token) {
    auto decision = [&]() -> boost::optional<MigrationDecisionEnum> {
        {
            stdx::lock_guard lk(_mutex);
            if (_isCommitOrAbortState(lk)) {
                return (_stateDoc.getState() == ShardMergeRecipientStateEnum::kCommitted)
                    ? MigrationDecisionEnum::kCommitted
                    : MigrationDecisionEnum::kAborted;
            }
        }
        if (MONGO_unlikely(autoRecipientForgetMigrationAbort.shouldFail())) {
            return MigrationDecisionEnum::kAborted;
        }
        return boost::none;
    }();

    if (decision) {
        auto opCtx = cc().makeOperationContext();
        onReceiveRecipientForgetMigration(opCtx.get(), *decision);
    }

    LOGV2_DEBUG(7339759,
                2,
                "Waiting to receive 'recipientForgetMigration' command.",
                "migrationId"_attr = _migrationUuid);

    return _receivedRecipientForgetMigrationPromise.getFuture()
        .semi()
        .thenRunOn(**_scopedExecutor)
        .then([this, self = shared_from_this()](MigrationDecisionEnum decision) {
            return _durablyPersistCommitAbortDecision(decision);
        })
        .then([this, self = shared_from_this()] { _dropTempCollections(); })
        .then([this, self = shared_from_this(), token] {
            // Note marking the keys as garbage collectable is not atomic with marking the
            // state document garbage collectable, so an interleaved failover can lead the
            // keys to be deleted before the state document has an expiration date. This is
            // acceptable because the decision to forget a migration is not reversible.
            return tenant_migration_util::markExternalKeysAsGarbageCollectable(
                _serviceContext,
                _scopedExecutor,
                _recipientService->getInstanceCleanupExecutor(),
                _migrationUuid,
                token);
        })
        .then([this, self = shared_from_this()] { return _markStateDocAsGarbageCollectable(); })
        .then([this, self = shared_from_this()] {
            stdx::lock_guard lk(_mutex);
            setPromiseOkifNotReady(lk, _forgetMigrationDurablePromise);
        })
        .semi();
}

SemiFuture<void> ShardMergeRecipientService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    _scopedExecutor = executor;
    _backupCursorExecutor = **_scopedExecutor;
    auto scopedOutstandingMigrationCounter =
        TenantMigrationStatistics::get(_serviceContext)->getScopedOutstandingReceivingCount();

    LOGV2(7339712,
          "Starting shard merge recipient instance: ",
          "migrationId"_attr = getMigrationUUID(),
          "connectionString"_attr = _donorConnectionString,
          "readPreference"_attr = _readPreference);

    pauseBeforeRunTenantMigrationRecipientInstance.pauseWhileSet();

    return ExecutorFuture(**executor)
        .then([this, self = shared_from_this()] {
            pauseAfterRunTenantMigrationRecipientInstance.pauseWhileSet();
            return _initializeAndDurablyPersistStateDoc();
        })
        .then([this, self = shared_from_this(), token] { return _prepareForMigration(token); })
        .then([this, self = shared_from_this(), token] {
            return _startMigrationIfSafeToRunwithCurrentFCV(token);
        })
        .then([this, self = shared_from_this()] { return _waitForMigrationToComplete(); })
        .thenRunOn(_recipientService->getInstanceCleanupExecutor())
        .onCompletion([this, self = shared_from_this()](
                          StatusOrStatusWith<TenantOplogApplier::OpTimePair> applierStatus) {
            // Note: The tenant oplog applier does not normally stop by itself on success. It
            // completes only on errors or on external interruption (e.g. by shutDown/stepDown or by
            // recipientForgetMigration command). So, errored completion status doesn't always mean
            // migration wasn't success.
            auto status = overrideMigrationErrToInterruptStatusIfNeeded(
                _migrationUuid, applierStatus.getStatus(), _interruptPromise.getFuture());

            LOGV2(7339713,
                  "Shard merge recipient instance: Migration completed.",
                  "migrationId"_attr = getMigrationUUID(),
                  "completionStatus"_attr = status);

            if (MONGO_unlikely(hangBeforeTaskCompletion.shouldFail())) {
                LOGV2(7339714,
                      "Shard merge recipient instance: hangBeforeTaskCompletion failpoint "
                      "enabled");
                hangBeforeTaskCompletion.pauseWhileSet();
            }

            _cleanupOnMigrationCompletion(status);
        })
        .thenRunOn(**_scopedExecutor)
        .then([this, self = shared_from_this(), token] {
            return _waitForForgetMigrationThenMarkMigrationGarbageCollectable(token);
        })
        .then([this, self = shared_from_this(), token] {
            return _waitForGarbageCollectionDelayThenDeleteStateDoc(token);
        })
        .thenRunOn(_recipientService->getInstanceCleanupExecutor())
        .onCompletion([this,
                       self = shared_from_this(),
                       scopedCounter{std::move(scopedOutstandingMigrationCounter)}](Status status) {
            // we won't don't want the errors
            // happened in the garbage collection stage to be replaced with interrupt errors due to
            // on receive of 'recipientForgetMigration' command but still want to replace with
            // failover/shutdown interrupt errors.
            status = overrideMigrationErrToInterruptStatusIfNeeded(_migrationUuid, status);
            if (status.isOK())
                return;

            LOGV2(7339715,
                  "Shard merge recipient instance not marked to be garbage collectable",
                  "migrationId"_attr = getMigrationUUID(),
                  "status"_attr = status);

            // We should only hit here on a stepdown or shudDown errors.
            invariant(ErrorCodes::isShutdownError(status) || ErrorCodes::isNotPrimaryError(status));

            stdx::lock_guard lk(_mutex);
            setPromiseErrorifNotReady(lk, _forgetMigrationDurablePromise, status);
        })
        .semi();
}

SemiFuture<void> ShardMergeRecipientService::Instance::_removeStateDoc(
    const CancellationToken& token) {
    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               pauseTenantMigrationRecipientBeforeDeletingStateDoc.pauseWhileSet(opCtx);

               PersistentTaskStore<ShardMergeRecipientDocument> store(_stateDocumentsNS);
               store.remove(
                   opCtx,
                   BSON(ShardMergeRecipientDocument::kIdFieldName << _migrationUuid),
                   WriteConcernOptions(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)));
               LOGV2(7339716,
                     "shard merge recipient state document is deleted",
                     "migrationId"_attr = _migrationUuid);
           })
        .until([](Status status) { return status.isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**_scopedExecutor, token)
        .semi();
}

SemiFuture<void>
ShardMergeRecipientService::Instance::_waitForGarbageCollectionDelayThenDeleteStateDoc(
    const CancellationToken& token) {
    stdx::lock_guard<Latch> lg(_mutex);
    LOGV2(7339717,
          "Waiting for garbage collection delay before deleting state document",
          "migrationId"_attr = _migrationUuid,
          "expireAt"_attr = *_stateDoc.getExpireAt());

    return (**_scopedExecutor)
        ->sleepUntil(*_stateDoc.getExpireAt(), token)
        .then([this, self = shared_from_this(), token]() {
            LOGV2(7339718,
                  "Deleting shard merge recipient state document",
                  "migrationId"_attr = _migrationUuid);
            return _removeStateDoc(token);
        })
        .semi();
}

const UUID& ShardMergeRecipientService::Instance::getMigrationUUID() const {
    return _migrationUuid;
}

ShardMergeRecipientDocument ShardMergeRecipientService::Instance::getStateDoc() const {
    stdx::lock_guard lk(_mutex);
    return _stateDoc;
}

}  // namespace repl
}  // namespace mongo
