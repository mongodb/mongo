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


#include "mongo/platform/basic.h"

#include <boost/filesystem/operations.hpp>
#include <fmt/format.h>

#include "mongo/base/checked_cast.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/config.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/write_ops_exec.h"
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
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/tenant_migration_recipient_entry_helpers.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_migration_statistics.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_import.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern_options.h"
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
const std::string kTTLIndexName = "TenantMigrationRecipientTTLIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());
constexpr StringData kOplogBufferPrefix = "repl.migration.oplog_"_sd;
constexpr int kBackupCursorFileFetcherRetryAttempts = 10;

NamespaceString getOplogBufferNs(const UUID& migrationUUID) {
    return NamespaceString(NamespaceString::kConfigDb,
                           kOplogBufferPrefix + migrationUUID.toString());
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

}  // namespace

// A convenient place to set test-specific parameters.
MONGO_FAIL_POINT_DEFINE(pauseBeforeRunTenantMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(pauseAfterRunTenantMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(skipTenantMigrationRecipientAuth);
MONGO_FAIL_POINT_DEFINE(skipComparingRecipientAndDonorFCV);
MONGO_FAIL_POINT_DEFINE(autoRecipientForgetMigration);
MONGO_FAIL_POINT_DEFINE(skipFetchingCommittedTransactions);
MONGO_FAIL_POINT_DEFINE(skipFetchingRetryableWritesEntriesBeforeStartOpTime);

// Fails before waiting for the state doc to be majority replicated.
MONGO_FAIL_POINT_DEFINE(failWhilePersistingTenantMigrationRecipientInstanceStateDoc);
MONGO_FAIL_POINT_DEFINE(fpAfterPersistingTenantMigrationRecipientInstanceStateDoc);
MONGO_FAIL_POINT_DEFINE(fpBeforeFetchingDonorClusterTimeKeys);
MONGO_FAIL_POINT_DEFINE(fpAfterConnectingTenantMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(fpAfterRecordingRecipientPrimaryStartingFCV);
MONGO_FAIL_POINT_DEFINE(fpAfterComparingRecipientAndDonorFCV);
MONGO_FAIL_POINT_DEFINE(fpAfterRetrievingStartOpTimesMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(fpSetSmallAggregationBatchSize);
MONGO_FAIL_POINT_DEFINE(fpBeforeWaitingForRetryableWritePreFetchMajorityCommitted);
MONGO_FAIL_POINT_DEFINE(pauseAfterRetrievingRetryableWritesBatch);
MONGO_FAIL_POINT_DEFINE(fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime);
MONGO_FAIL_POINT_DEFINE(fpAfterStartingOplogFetcherMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(setTenantMigrationRecipientInstanceHostTimeout);
MONGO_FAIL_POINT_DEFINE(pauseAfterRetrievingLastTxnMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(fpBeforeMarkingCloneSuccess);
MONGO_FAIL_POINT_DEFINE(fpBeforeFetchingCommittedTransactions);
MONGO_FAIL_POINT_DEFINE(fpAfterFetchingCommittedTransactions);
MONGO_FAIL_POINT_DEFINE(fpAfterStartingOplogApplierMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(fpBeforeFulfillingDataConsistentPromise);
MONGO_FAIL_POINT_DEFINE(fpAfterDataConsistentMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(fpBeforePersistingRejectReadsBeforeTimestamp);
MONGO_FAIL_POINT_DEFINE(fpAfterWaitForRejectReadsBeforeTimestamp);
MONGO_FAIL_POINT_DEFINE(hangBeforeTaskCompletion);
MONGO_FAIL_POINT_DEFINE(fpAfterReceivingRecipientForgetMigration);
MONGO_FAIL_POINT_DEFINE(hangAfterCreatingRSM);
MONGO_FAIL_POINT_DEFINE(skipRetriesWhenConnectingToDonorHost);
MONGO_FAIL_POINT_DEFINE(fpBeforeDroppingOplogBufferCollection);
MONGO_FAIL_POINT_DEFINE(fpWaitUntilTimestampMajorityCommitted);
MONGO_FAIL_POINT_DEFINE(hangAfterUpdatingTransactionEntry);
MONGO_FAIL_POINT_DEFINE(fpBeforeAdvancingStableTimestamp);
MONGO_FAIL_POINT_DEFINE(hangMigrationBeforeRetryCheck);

namespace {
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

    JournalListener* getReplicationJournalListener() final {
        MONGO_UNREACHABLE;
    }
};


}  // namespace

TenantMigrationRecipientService::TenantMigrationRecipientService(
    ServiceContext* const serviceContext)
    : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}

StringData TenantMigrationRecipientService::getServiceName() const {
    return kTenantMigrationRecipientServiceName;
}

NamespaceString TenantMigrationRecipientService::getStateDocumentsNS() const {
    return NamespaceString::kTenantMigrationRecipientsNamespace;
}

ThreadPool::Limits TenantMigrationRecipientService::getThreadPoolLimits() const {
    ThreadPool::Limits limits;
    limits.maxThreads = maxTenantMigrationRecipientThreadPoolSize;
    return limits;
}

void TenantMigrationRecipientService::abortAllMigrations(OperationContext* opCtx) {
    LOGV2(5356303, "Aborting all tenant migrations on recipient");
    auto instances = getAllInstances(opCtx);
    for (auto& instance : instances) {
        auto typedInstance =
            checked_pointer_cast<TenantMigrationRecipientService::Instance>(instance);
        typedInstance->cancelMigration();
    }
}

ExecutorFuture<void> TenantMigrationRecipientService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
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
        .until([token](Status status) { return status.isOK() || token.isCanceled(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

void TenantMigrationRecipientService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialStateDoc,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {
    auto tenantId = initialStateDoc["tenantId"].valueStringData();

    for (auto& instance : existingInstances) {
        auto existingTypedInstance =
            checked_cast<const TenantMigrationRecipientService::Instance*>(instance);
        auto existingState = existingTypedInstance->getState();
        auto isDone = existingState.getState() == TenantMigrationRecipientStateEnum::kDone &&
            existingState.getExpireAt();

        uassert(ErrorCodes::ConflictingOperationInProgress,
                "an existing shard merge is in progress",
                isDone ||
                    (existingTypedInstance->getProtocol() != MigrationProtocolEnum::kShardMerge &&
                     existingState.getProtocol() != MigrationProtocolEnum::kShardMerge));

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "tenant " << tenantId << " is already migrating",
                isDone || existingTypedInstance->getTenantId() != tenantId);
    }
}

std::shared_ptr<PrimaryOnlyService::Instance> TenantMigrationRecipientService::constructInstance(
    BSONObj initialStateDoc) {
    return std::make_shared<TenantMigrationRecipientService::Instance>(
        _serviceContext, this, initialStateDoc);
}

TenantMigrationRecipientService::Instance::Instance(
    ServiceContext* const serviceContext,
    const TenantMigrationRecipientService* recipientService,
    BSONObj stateDoc)
    : PrimaryOnlyService::TypedInstance<Instance>(),
      _serviceContext(serviceContext),
      _recipientService(recipientService),
      _stateDoc(TenantMigrationRecipientDocument::parse(IDLParserErrorContext("recipientStateDoc"),
                                                        stateDoc)),
      _tenantId(_stateDoc.getTenantId().toString()),
      _protocol(_stateDoc.getProtocol().value_or(MigrationProtocolEnum::kMultitenantMigrations)),
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
                      "Cannot run tenant migration with x509 authentication as SSL is not enabled",
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

boost::optional<BSONObj> TenantMigrationRecipientService::Instance::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder bob;

    stdx::lock_guard lk(_mutex);
    bob.append("desc", "tenant recipient migration");
    _migrationUuid.appendToBuilder(&bob, "instanceID"_sd);
    bob.append("tenantId", _stateDoc.getTenantId());
    bob.append("donorConnectionString", _stateDoc.getDonorConnectionString());
    bob.append("readPreference", _stateDoc.getReadPreference().toInnerBSON());
    bob.append("state", _stateDoc.getState());
    bob.append("dataSyncCompleted", _dataSyncCompletionPromise.getFuture().isReady());
    bob.append("migrationCompleted", _taskCompletionPromise.getFuture().isReady());
    bob.append("numRestartsDueToDonorConnectionFailure",
               _stateDoc.getNumRestartsDueToDonorConnectionFailure());
    bob.append("numRestartsDueToRecipientFailure", _stateDoc.getNumRestartsDueToRecipientFailure());

    if (_tenantAllDatabaseCloner) {
        auto stats = _tenantAllDatabaseCloner->getStats();
        bob.append("approxTotalDataSize", stats.approxTotalDataSize);
        bob.append("approxTotalBytesCopied", stats.approxTotalBytesCopied);

        long long elapsedMillis = duration_cast<Milliseconds>(Date_t::now() - stats.start).count();
        bob.append("totalReceiveElapsedMillis", elapsedMillis);

        // Perform the multiplication first to avoid rounding errors, and add one to avoid division
        // by 0.
        long long timeRemainingMillis =
            ((stats.approxTotalDataSize - stats.approxTotalBytesCopied) * elapsedMillis) /
            (stats.approxTotalBytesCopied + 1);

        bob.append("remainingReceiveEstimatedMillis", timeRemainingMillis);

        BSONObjBuilder dbsBuilder(bob.subobjStart("databases"));
        _tenantAllDatabaseCloner->getStats().append(&dbsBuilder);
        dbsBuilder.doneFast();
    }

    if (_stateDoc.getStartFetchingDonorOpTime())
        _stateDoc.getStartFetchingDonorOpTime()->append(&bob, "startFetchingDonorOpTime");
    if (_stateDoc.getStartApplyingDonorOpTime())
        _stateDoc.getStartApplyingDonorOpTime()->append(&bob, "startApplyingDonorOpTime");
    if (_stateDoc.getDataConsistentStopDonorOpTime())
        _stateDoc.getDataConsistentStopDonorOpTime()->append(&bob, "dataConsistentStopDonorOpTime");
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

void TenantMigrationRecipientService::Instance::checkIfOptionsConflict(
    const BSONObj& options) const {
    auto stateDoc = TenantMigrationRecipientDocument::parse(
        IDLParserErrorContext("recipientStateDoc"), options);

    invariant(stateDoc.getId() == _migrationUuid);

    if (stateDoc.getProtocol() != _protocol || stateDoc.getTenantId() != _tenantId ||
        stateDoc.getDonorConnectionString() != _donorConnectionString ||
        !stateDoc.getReadPreference().equals(_readPreference) ||
        stateDoc.getRecipientCertificateForDonor() != _recipientCertificateForDonor) {
        uasserted(ErrorCodes::ConflictingOperationInProgress,
                  str::stream() << "Found active migration for migrationId \""
                                << _migrationUuid.toBSON() << "\" with different options "
                                << tenant_migration_util::redactStateDoc(_stateDoc.toBSON()));
    }
}

OpTime TenantMigrationRecipientService::Instance::waitUntilMigrationReachesConsistentState(
    OperationContext* opCtx) const {
    return _dataConsistentPromise.getFuture().get(opCtx);
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

OpTime
TenantMigrationRecipientService::Instance::waitUntilMigrationReachesReturnAfterReachingTimestamp(
    OperationContext* opCtx, const Timestamp& returnAfterReachingTimestamp) {
    // This gives assurance that _tenantOplogApplier pointer won't be empty, and that it has been
    // started. Additionally, we must have finished processing the recipientSyncData command that
    // waits on _dataConsistentPromise.
    _dataConsistentPromise.getFuture().get(opCtx);

    auto getWaitOpTimeFuture = [&]() {
        stdx::unique_lock lk(_mutex);
        // In the event of a donor failover, it is possible that a new donor has stepped up and
        // initiated this 'recipientSyncData' cmd. Make sure the recipient is not in the middle of
        // restarting the oplog applier to retry the future chain.
        //
        // For shard merge protocol, we start tenant oplog applier after recipient informs donor,
        // the data is in consistent state. So, there is a possibility, recipient might receive
        // recipientSyncData cmd with `returnAfterReachingDonorTimestamp` from donor before the
        // recipient has started the tenant oplog applier.
        opCtx->waitForConditionOrInterrupt(_oplogApplierReadyCondVar, lk, [&] {
            return _oplogApplierReady || _dataSyncCompletionPromise.getFuture().isReady();
        });
        if (_dataSyncCompletionPromise.getFuture().isReady()) {
            // When the data sync is done, we reset _tenantOplogApplier, so just throw the data sync
            // completion future result.
            _dataSyncCompletionPromise.getFuture().get();
            MONGO_UNREACHABLE;
        }

        // Sanity checks.
        invariant(_tenantOplogApplier);
        auto state = _stateDoc.getState();
        uassert(ErrorCodes::IllegalOperation,
                str::stream()
                    << "Failed to wait for the donor timestamp to be majority committed due to"
                       "conflicting tenant migration state, migration uuid: "
                    << getMigrationUUID() << " , current state: "
                    << TenantMigrationRecipientState_serializer(state) << " , expected state: "
                    << TenantMigrationRecipientState_serializer(
                           TenantMigrationRecipientStateEnum::kConsistent)
                    << ".",
                state == TenantMigrationRecipientStateEnum::kConsistent);

        return _tenantOplogApplier->getNotificationForOpTime(
            OpTime(returnAfterReachingTimestamp, OpTime::kUninitializedTerm));
    };

    auto waitOpTimeFuture = getWaitOpTimeFuture();
    fpWaitUntilTimestampMajorityCommitted.pauseWhileSet();
    auto swDonorRecipientOpTimePair = waitOpTimeFuture.getNoThrow();

    auto status = swDonorRecipientOpTimePair.getStatus();

    // A cancellation error may occur due to an interrupt. If that is the case, replace the error
    // code with the interrupt code, the true reason for interruption.
    if (ErrorCodes::isCancellationError(status)) {
        stdx::lock_guard lk(_mutex);
        if (!_taskState.getInterruptStatus().isOK()) {
            status = _taskState.getInterruptStatus();
        }
    }

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
    uassertStatusOK(tenantMigrationRecipientEntryHelpers::updateStateDoc(opCtx, stateDoc));
    auto lastOpAfterUpdate = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    auto replCoord = repl::ReplicationCoordinator::get(_serviceContext);
    if (lastOpBeforeUpdate == lastOpAfterUpdate) {
        // updateStateDoc was a no-op, but we still must ensure it's all-replicated.
        lastOpAfterUpdate = uassertStatusOK(replCoord->getLatestWriteOpTime(opCtx));
        LOGV2(6096900,
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

std::unique_ptr<DBClientConnection> TenantMigrationRecipientService::Instance::_connectAndAuth(
    const HostAndPort& serverAddress, StringData applicationName) {
    auto swClientBase = ConnectionString(serverAddress)
                            .connect(applicationName,
                                     0 /* socketTimeout */,
                                     nullptr /* uri */,
                                     nullptr /* apiParameters */,
                                     _transientSSLParams ? &_transientSSLParams.get() : nullptr);
    if (!swClientBase.isOK()) {
        LOGV2_ERROR(4880400,
                    "Failed to connect to migration donor",
                    "tenantId"_attr = getTenantId(),
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
        uassertStatusOK(
            replAuthenticate(clientBase)
                .withContext(str::stream()
                             << "TenantMigrationRecipientService failed to authenticate to "
                             << serverAddress));
    } else if (MONGO_likely(!skipTenantMigrationRecipientAuth.shouldFail())) {
        client->auth(auth::createInternalX509AuthDocument());
    }

    return client;
}

OpTime TenantMigrationRecipientService::Instance::_getDonorMajorityOpTime(
    std::unique_ptr<mongo::DBClientConnection>& client) {
    auto oplogOpTimeFields =
        BSON(OplogEntry::kTimestampFieldName << 1 << OplogEntry::kTermFieldName << 1);
    FindCommandRequest findCmd{NamespaceString::kRsOplogNamespace};
    findCmd.setSort(BSON("$natural" << -1));
    findCmd.setProjection(oplogOpTimeFields);
    findCmd.setReadConcern(ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    auto majorityOpTimeBson = client->findOne(
        std::move(findCmd), ReadPreferenceSetting{ReadPreference::SecondaryPreferred});
    uassert(5272003, "Found no entries in the remote oplog", !majorityOpTimeBson.isEmpty());

    auto majorityOpTime = uassertStatusOK(OpTime::parseFromOplogEntry(majorityOpTimeBson));
    return majorityOpTime;
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_createAndConnectClients() {
    LOGV2_DEBUG(4880401,
                1,
                "Recipient migration service connecting clients",
                "tenantId"_attr = getTenantId(),
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
        LOGV2(5272004, "hangAfterCreatingRSM failpoint enabled");
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
                       LOGV2(5272002,
                             "Attempting to connect to donor host",
                             "donorHost"_attr = serverAddress,
                             "tenantId"_attr = getTenantId(),
                             "migrationId"_attr = getMigrationUUID());
                       // Application name is constructed such that it doesn't exceeds
                       // kMaxApplicationNameByteLength (128 bytes).
                       // "TenantMigration_" (16 bytes) + <tenantId> (61 bytes) + "_" (1 byte) +
                       // <migrationUuid> (36 bytes) =  114 bytes length.
                       // Note: Since the total length of tenant database name (<tenantId>_<user
                       // provided db name>) can't exceed 63 bytes and the user provided db name
                       // should be at least one character long, the maximum length of tenantId can
                       // only be 61 bytes.
                       auto applicationName =
                           "TenantMigration_" + getTenantId() + "_" + getMigrationUUID().toString();

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
                           const auto now = getGlobalServiceContext()->getFastClockSource()->now();
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
                           const auto now = getGlobalServiceContext()->getFastClockSource()->now();
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

                       // Application name is constructed such that it doesn't exceed
                       // kMaxApplicationNameByteLength (128 bytes).
                       // "TenantMigration_" (16 bytes) + <tenantId> (61 bytes) + "_" (1 byte) +
                       // <migrationUuid> (36 bytes) + _oplogFetcher" (13 bytes) =  127 bytes
                       // length.
                       applicationName += "_oplogFetcher";
                       auto oplogFetcherClient = _connectAndAuth(serverAddress, applicationName);
                       return ConnectionPair(std::move(client), std::move(oplogFetcherClient));
                   })
                   .then([this, self = shared_from_this()](ConnectionPair connectionPair) {
                       stdx::lock_guard lk(_mutex);
                       if (_taskState.isInterrupted()) {
                           uassertStatusOK(_taskState.getInterruptStatus());
                       }

                       _client = std::move(connectionPair.first);
                       _oplogFetcherClient = std::move(connectionPair.second);
                   });
           })
        .until([this, self = shared_from_this(), kDelayedMajorityOpTimeErrorCode](
                   const Status& status) {
            if (status.isOK()) {
                return true;
            }

            LOGV2_ERROR(4880404,
                        "Connecting to donor failed",
                        "tenantId"_attr = getTenantId(),
                        "migrationId"_attr = getMigrationUUID(),
                        "error"_attr = status);

            // Make sure we don't end up with a partially initialized set of connections.
            stdx::lock_guard lk(_mutex);
            _client = nullptr;
            _oplogFetcherClient = nullptr;

            // If the future chain has been interrupted, stop retrying.
            if (_taskState.isInterrupted()) {
                return true;
            }

            if (MONGO_unlikely(skipRetriesWhenConnectingToDonorHost.shouldFail())) {
                LOGV2(5425600,
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

void TenantMigrationRecipientService::Instance::_excludeDonorHost(WithLock,
                                                                  const HostAndPort& host,
                                                                  Date_t until) {
    LOGV2_DEBUG(5271800,
                2,
                "Excluding donor host",
                "donorHost"_attr = host,
                "until"_attr = until.toString());

    _excludedDonorHosts.emplace_back(std::make_pair(host, until));
}

std::vector<HostAndPort> TenantMigrationRecipientService::Instance::_getExcludedDonorHosts(
    WithLock lk) {
    const auto now = getGlobalServiceContext()->getFastClockSource()->now();

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

SemiFuture<void> TenantMigrationRecipientService::Instance::_initializeStateDoc(WithLock) {
    // If the instance state has a startAt field, then the instance is restarted by step up. So,
    // skip persisting the state doc. And, PrimaryOnlyService::onStepUp() waits for majority commit
    // of the primary no-op oplog entry written by the node in the newer term before scheduling the
    // Instance::run(). So, it's also safe to assume that instance's state document written in an
    // older term on disk won't get rolled back for step up case.
    if (_stateDoc.getStartAt()) {
        return SemiFuture<void>::makeReady();
    }

    LOGV2_DEBUG(5081400,
                2,
                "Recipient migration service initializing state document",
                "tenantId"_attr = getTenantId(),
                "migrationId"_attr = getMigrationUUID(),
                "connectionString"_attr = _donorConnectionString,
                "readPreference"_attr = _readPreference);

    if (_stateDoc.getState() != TenantMigrationRecipientStateEnum::kDone) {
        _stateDoc.setState(TenantMigrationRecipientStateEnum::kStarted);
    } else {
        // If we don't have a startAt field, we shouldn't have an expireAt either.
        // If the state is 'kDone' without the expireAt field, it means a recipientForgetMigration
        // command is received before a recipientSyncData command or after the state doc is garbage
        // collected. We want to re-initialize the state doc but immediately mark it garbage
        // collectable to account for delayed recipientSyncData commands.
        invariant(!_stateDoc.getExpireAt());
    }
    // Persist the state doc before starting the data sync.
    _stateDoc.setStartAt(getGlobalServiceContext()->getFastClockSource()->now());

    return ExecutorFuture(**_scopedExecutor)
        .then([this, self = shared_from_this(), stateDoc = _stateDoc] {
            auto opCtx = cc().makeOperationContext();
            {
                Lock::ExclusiveLock stateDocInsertLock(
                    opCtx.get(), opCtx->lockState(), _recipientService->_stateDocInsertMutex);
                uassertStatusOK(
                    tenantMigrationRecipientEntryHelpers::insertStateDoc(opCtx.get(), stateDoc));
            }

            if (MONGO_unlikely(
                    failWhilePersistingTenantMigrationRecipientInstanceStateDoc.shouldFail())) {
                LOGV2(4878500, "Persisting state doc failed due to fail point enabled.");
                uasserted(
                    ErrorCodes::NotWritablePrimary,
                    "Persisting state doc failed - "
                    "'failWhilePersistingTenantMigrationRecipientInstanceStateDoc' fail point "
                    "active");
            }

            // Wait for the state doc to be majority replicated to make sure that the state doc
            // doesn't rollback.
            auto writeOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            return WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(writeOpTime, CancellationToken::uncancelable());
        })
        .semi();
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_killBackupCursor() {
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
            LOGV2_INFO(6113421,
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

            auto scheduleResult =
                (_recipientService->getInstanceCleanupExecutor())
                    ->scheduleRemoteCommand(
                        request, [](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                            if (!args.response.isOK()) {
                                LOGV2_WARNING(6113005,
                                              "killCursors command task failed",
                                              "error"_attr = redact(args.response.status));
                                return;
                            }
                            auto status = getStatusFromCommandResult(args.response.data);
                            if (status.isOK()) {
                                LOGV2_INFO(6113415, "Killed backup cursor");
                            } else {
                                LOGV2_WARNING(6113006,
                                              "killCursors command failed",
                                              "error"_attr = redact(status));
                            }
                        });
            if (!scheduleResult.isOK()) {
                LOGV2_WARNING(6113004,
                              "Failed to run killCursors command on backup cursor",
                              "status"_attr = scheduleResult.getStatus());
            }
        })
        .semi();
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_openBackupCursor(
    const CancellationToken& token) {
    stdx::lock_guard lk(_mutex);
    LOGV2_DEBUG(6113000,
                1,
                "Trying to open backup cursor on donor primary",
                "migrationId"_attr = _stateDoc.getId(),
                "donorConnectionString"_attr = _stateDoc.getDonorConnectionString());
    const auto cmdObj = [] {
        AggregateCommandRequest aggRequest(
            NamespaceString::makeCollectionlessAggregateNSS(NamespaceString::kAdminDb),
            {BSON("$backupCursor" << BSONObj())});
        // We must set a writeConcern on internal commands.
        aggRequest.setWriteConcern(WriteConcernOptions());
        return aggRequest.toBSON(BSONObj());
    }();

    auto fetchStatus = std::make_shared<boost::optional<Status>>();
    auto uniqueMetadataInfo = std::make_unique<boost::optional<shard_merge_utils::MetadataInfo>>();
    auto fetcherCallback =
        [
            this,
            self = shared_from_this(),
            fetchStatus,
            metadataInfoPtr = uniqueMetadataInfo.get(),
            token
        ](const Fetcher::QueryResponseStatus& dataStatus,
          Fetcher::NextAction* nextAction,
          BSONObjBuilder* getMoreBob) noexcept {
        try {
            uassertStatusOK(dataStatus);
            uassert(ErrorCodes::CallbackCanceled, "backup cursor interrupted", !token.isCanceled());

            auto uniqueOpCtx = cc().makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            const auto& data = dataStatus.getValue();
            for (const BSONObj& doc : data.documents) {
                if (doc["metadata"]) {
                    // First batch must contain the metadata.
                    const auto& metadata = doc["metadata"].Obj();
                    auto checkpointTimestamp = metadata["checkpointTimestamp"].timestamp();

                    LOGV2_INFO(6113001,
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

                    invariant(metadataInfoPtr && !*metadataInfoPtr);
                    (*metadataInfoPtr) = shard_merge_utils::MetadataInfo::constructMetadataInfo(
                        getMigrationUUID(), _client->getServerAddress(), metadata);
                } else {
                    LOGV2_DEBUG(6113002,
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
            LOGV2_ERROR(6409801,
                        "Error fetching backup cursor entries",
                        "migrationId"_attr = getMigrationUUID(),
                        "error"_attr = ex.toString());
            *fetchStatus = ex.toStatus();
        }
    };

    _donorFilenameBackupCursorFileFetcher = std::make_unique<Fetcher>(
        (**_scopedExecutor).get(),
        _client->getServerHostAndPort(),
        NamespaceString::kAdminDb.toString(),
        cmdObj,
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
                uasserted(6113007, "Internal error running cursor callback in command");
            }

            uassertStatusOK(fetchStatus->get());
        })
        .semi();
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_openBackupCursorWithRetry(
    const CancellationToken& token) {
    return AsyncTry([this, self = shared_from_this(), token] { return _openBackupCursor(token); })
        .until([this, self = shared_from_this()](Status status) {
            if (status == ErrorCodes::BackupCursorOpenConflictWithCheckpoint) {
                LOGV2_INFO(6113008,
                           "Retrying backup cursor creation after transient error",
                           "migrationId"_attr = getMigrationUUID(),
                           "status"_attr = status);
                // A checkpoint took place while opening a backup cursor. We
                // should retry and *not* cancel migration.
                return false;
            }

            return true;
        })
        .on(**_scopedExecutor, token)
        .semi();
}

const BackupCursorInfo& TenantMigrationRecipientService::Instance::_getDonorBackupCursorInfo(
    WithLock) const {
    stdx::lock_guard<TenantMigrationSharedData> sharedDatalk(*_sharedData);
    return _sharedData->getDonorBackupCursorInfo(sharedDatalk);
}

void TenantMigrationRecipientService::Instance::_keepBackupCursorAlive(
    const CancellationToken& token) {
    LOGV2_DEBUG(6113200,
                1,
                "Starting periodic 'getMore' requests to keep "
                "backup cursor alive.",
                "migrationId"_attr = getMigrationUUID());

    stdx::lock_guard lk(_mutex);
    _backupCursorKeepAliveCancellation = CancellationSource(token);
    auto& donorBackupCursorInfo = _getDonorBackupCursorInfo(lk);
    _backupCursorKeepAliveFuture =
        shard_merge_utils::keepBackupCursorAlive(_backupCursorKeepAliveCancellation,
                                                 **_scopedExecutor,
                                                 _client->getServerHostAndPort(),
                                                 donorBackupCursorInfo.cursorId,
                                                 donorBackupCursorInfo.nss);
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_enterLearnedFilenamesState() {
    stdx::lock_guard lk(_mutex);
    _stateDoc.setState(TenantMigrationRecipientStateEnum::kLearnedFilenames);
    return _updateStateDocForMajority(lk);
}

boost::optional<OpTime> TenantMigrationRecipientService::Instance::_getOldestActiveTransactionAt(
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
    LOGV2_DEBUG(4880602,
                2,
                "Transaction table entry for earliest transaction that was open at the read "
                "concern majority optime on remote node (may be empty)",
                "migrationId"_attr = getMigrationUUID(),
                "tenantId"_attr = _stateDoc.getTenantId(),
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

SemiFuture<void> TenantMigrationRecipientService::Instance::_getStartOpTimesFromDonor() {
    OpTime startApplyingDonorOpTime;
    stdx::unique_lock lk(_mutex);
    // Get the last oplog entry at the read concern majority optime in the remote oplog. It
    // does not matter which tenant it is for.
    if (_sharedData->getResumePhase() != ResumePhase::kNone) {
        // We are resuming a migration.
        return SemiFuture<void>::makeReady();
    }

    if (getProtocol() == MigrationProtocolEnum::kShardMerge) {
        startApplyingDonorOpTime =
            OpTime(_getDonorBackupCursorInfo(lk).checkpointTimestamp, OpTime::kUninitializedTerm);
        lk.unlock();
    } else {
        lk.unlock();
        startApplyingDonorOpTime = _getDonorMajorityOpTime(_client);
        LOGV2_DEBUG(4880600,
                    2,
                    "Found last oplog entry at read concern majority optime on remote node",
                    "migrationId"_attr = getMigrationUUID(),
                    "tenantId"_attr = _stateDoc.getTenantId(),
                    "lastOplogEntry"_attr = startApplyingDonorOpTime.toBSON());
    }

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

AggregateCommandRequest
TenantMigrationRecipientService::Instance::_makeCommittedTransactionsAggregation() const {

    auto opCtx = cc().makeOperationContext();
    auto expCtx = makeExpressionContext(opCtx.get());

    Timestamp startApplyingDonorOpTime;
    {
        stdx::lock_guard lk(_mutex);
        invariant(_stateDoc.getStartApplyingDonorOpTime());
        startApplyingDonorOpTime = _stateDoc.getStartApplyingDonorOpTime().get().getTimestamp();
    }

    auto serializedPipeline =
        tenant_migration_util::createCommittedTransactionsPipelineForTenantMigrations(
            expCtx, startApplyingDonorOpTime, getTenantId())
            ->serializeToBson();

    AggregateCommandRequest aggRequest(NamespaceString::kSessionTransactionsTableNamespace,
                                       std::move(serializedPipeline));

    auto readConcern = repl::ReadConcernArgs(
        boost::optional<repl::ReadConcernLevel>(repl::ReadConcernLevel::kMajorityReadConcern));
    aggRequest.setReadConcern(readConcern.toBSONInner());

    aggRequest.setHint(BSON(SessionTxnRecord::kSessionIdFieldName << 1));
    aggRequest.setCursor(SimpleCursorOptions());
    // We must set a writeConcern on internal commands.
    aggRequest.setWriteConcern(WriteConcernOptions());

    return aggRequest;
}

void TenantMigrationRecipientService::Instance::_processCommittedTransactionEntry(
    const BSONObj& entry) {
    auto sessionTxnRecord =
        SessionTxnRecord::parse(IDLParserErrorContext("SessionTxnRecord"), entry);
    auto sessionId = sessionTxnRecord.getSessionId();
    auto txnNumber = sessionTxnRecord.getTxnNum();
    auto optTxnRetryCounter = sessionTxnRecord.getTxnRetryCounter();
    uassert(ErrorCodes::InvalidOptions,
            "txnRetryCounter is only supported in sharded clusters",
            !optTxnRetryCounter.has_value());

    auto uniqueOpCtx = cc().makeOperationContext();
    auto opCtx = uniqueOpCtx.get();

    // If the tenantMigrationRecipientInfo is set on the opCtx, we will set the
    // 'fromTenantMigration' field when writing oplog entries. That field is used to help recipient
    // secondaries determine if a no-op entry is related to a transaction entry.
    tenantMigrationRecipientInfo(opCtx) =
        boost::make_optional<TenantMigrationRecipientInfo>(getMigrationUUID());
    opCtx->setLogicalSessionId(sessionId);
    opCtx->setTxnNumber(txnNumber);
    opCtx->setInMultiDocumentTransaction();
    MongoDOperationContextSession ocs(opCtx);

    LOGV2_DEBUG(5351301,
                1,
                "Migration attempting to commit transaction",
                "sessionId"_attr = sessionId,
                "txnNumber"_attr = txnNumber,
                "txnRetryCounter"_attr = optTxnRetryCounter,
                "tenantId"_attr = getTenantId(),
                "migrationId"_attr = getMigrationUUID(),
                "entry"_attr = entry);

    auto txnParticipant = TransactionParticipant::get(opCtx);
    uassert(5351300,
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
    auto tenantNss = NamespaceString(getTenantId() + "_", "");
    noopEntry.setNss(tenantNss);
    // Write a fake applyOps with the tenantId as the namespace so that this will be picked
    // up by the committed transaction prefetch pipeline in subsequent migrations.
    noopEntry.setObject(
        BSON("applyOps" << BSON_ARRAY(BSON(OplogEntry::kNssFieldName << tenantNss.ns()))));
    noopEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());
    noopEntry.setSessionId(sessionId);
    noopEntry.setTxnNumber(txnNumber);
    noopEntry.getOperationSessionInfo().setTxnRetryCounter(optTxnRetryCounter);

    // Use the same wallclock time as the noop entry.
    sessionTxnRecord.setStartOpTime(boost::none);
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
        LOGV2(5351400, "hangAfterUpdatingTransactionEntry failpoint enabled");
        hangAfterUpdatingTransactionEntry.pauseWhileSet();
        if (data["failAfterHanging"].trueValue()) {
            // Simulate the sync source shutting down/restarting.
            uasserted(ErrorCodes::ShutdownInProgress,
                      "Throwing error due to hangAfterUpdatingTransactionEntry failpoint");
        }
    });
}

SemiFuture<void>
TenantMigrationRecipientService::Instance::_fetchCommittedTransactionsBeforeStartOpTime() {
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
                5351401,
                2,
                "Already completed fetching committed transactions from donor, skipping stage",
                "migrationId"_attr = getMigrationUUID(),
                "tenantId"_attr = getTenantId());
            return SemiFuture<void>::makeReady();
        }
    }

    auto aggRequest = _makeCommittedTransactionsAggregation();

    auto statusWith = DBClientCursor::fromAggregationRequest(
        _client.get(), std::move(aggRequest), true /* secondaryOk */, false /* useExhaust */);
    if (!statusWith.isOK()) {
        LOGV2_ERROR(5351100,
                    "Fetch committed transactions aggregation failed",
                    "error"_attr = statusWith.getStatus());
        uassertStatusOK(statusWith.getStatus());
    }

    auto cursor = statusWith.getValue().get();
    while (cursor->more()) {
        auto transactionEntry = cursor->next();
        _processCommittedTransactionEntry(transactionEntry);

        stdx::lock_guard lk(_mutex);
        if (_taskState.isInterrupted()) {
            uassertStatusOK(_taskState.getInterruptStatus());
        }
    }

    stdx::lock_guard lk(_mutex);
    _stateDoc.setCompletedUpdatingTransactionsBeforeStartOpTime(true);
    return ExecutorFuture(**_scopedExecutor)
        .then([this, self = shared_from_this(), stateDoc = _stateDoc] {
            auto opCtx = cc().makeOperationContext();
            uassertStatusOK(
                tenantMigrationRecipientEntryHelpers::updateStateDoc(opCtx.get(), stateDoc));
        })
        .semi();
}

void TenantMigrationRecipientService::Instance::_createOplogBuffer(WithLock,
                                                                   OperationContext* opCtx) {
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
TenantMigrationRecipientService::Instance::_fetchRetryableWritesOplogBeforeStartOpTime() {
    _stopOrHangOnFailPoint(&fpAfterRetrievingStartOpTimesMigrationRecipientInstance);
    if (MONGO_unlikely(
            skipFetchingRetryableWritesEntriesBeforeStartOpTime.shouldFail())) {  // Test-only.
        return SemiFuture<void>::makeReady();
    }

    {
        stdx::lock_guard lk(_mutex);
        if (_stateDoc.getCompletedFetchingRetryableWritesBeforeStartOpTime()) {
            LOGV2_DEBUG(5350800,
                        2,
                        "Already completed fetching retryable writes oplog entries from donor, "
                        "skipping stage",
                        "migrationId"_attr = getMigrationUUID(),
                        "tenantId"_attr = getTenantId());
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
        startFetchingTimestamp = _stateDoc.getStartFetchingDonorOpTime().get().getTimestamp();
    }

    LOGV2_DEBUG(5535300,
                1,
                "Pre-fetching retryable oplog entries before startFetchingTimstamp",
                "startFetchingTimestamp"_attr = startFetchingTimestamp,
                "tenantId"_attr = getTenantId(),
                "migrationId"_attr = getMigrationUUID());

    // Fetch the oplog chains of all retryable writes that occurred before startFetchingTimestamp
    // on this tenant.
    auto serializedPipeline =
        tenant_migration_util::createRetryableWritesOplogFetchingPipelineForTenantMigrations(
            expCtx, startFetchingTimestamp, getTenantId())
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
        {
            stdx::unique_lock lk(_mutex);
            if (_taskState.isInterrupted()) {
                uassertStatusOK(_taskState.getInterruptStatus());
            }
        }
    }

    // Do a majority read on the sync source to make sure the pre-fetch result exists on a
    // majority of nodes in the set. The timestamp we wait on is the donor's last applied
    // operationTime, which is guaranteed to be at batch boundary if the sync source is a
    // secondary. We do not check the rollbackId - rollback would lead to the sync source
    // closing connections so the migration would fail and retry.
    auto operationTime = cursor->getOperationTime();
    uassert(5663100,
            "Donor operationTime not available in retryable write pre-fetch result.",
            operationTime);
    LOGV2_DEBUG(5663101,
                1,
                "Waiting for retryable write pre-fetch result to be majority committed.",
                "operationTime"_attr = operationTime);

    fpBeforeWaitingForRetryableWritePreFetchMajorityCommitted.pauseWhileSet();

    BSONObj readResult;
    BSONObj cmd = ClonerUtils::buildMajorityWaitRequest(*operationTime);
    _client.get()->runCommand("admin", cmd, readResult, QueryOption_SecondaryOk);
    uassertStatusOKWithContext(
        getStatusFromCommandResult(readResult),
        "Failed to wait for retryable writes pre-fetch result majority committed");

    // Update _stateDoc to indicate that we've finished the retryable writes oplog entry fetching
    // stage.
    stdx::lock_guard lk(_mutex);
    _stateDoc.setCompletedFetchingRetryableWritesBeforeStartOpTime(true);
    return ExecutorFuture(**_scopedExecutor)
        .then([this, self = shared_from_this(), stateDoc = _stateDoc] {
            auto opCtx = cc().makeOperationContext();
            uassertStatusOK(
                tenantMigrationRecipientEntryHelpers::updateStateDoc(opCtx.get(), stateDoc));
        })
        .semi();
}

void TenantMigrationRecipientService::Instance::_startOplogFetcher() {
    _stopOrHangOnFailPoint(&fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime);

    auto opCtx = cc().makeOperationContext();
    OpTime startFetchOpTime;
    auto resumingFromOplogBuffer = false;

    {
        stdx::lock_guard lk(_mutex);
        _dataReplicatorExternalState =
            std::make_unique<DataReplicatorExternalStateTenantMigration>();
        startFetchOpTime = *_stateDoc.getStartFetchingDonorOpTime();
    }

    if (_sharedData->getResumePhase() != ResumePhase::kNone) {
        // If the oplog buffer already contains fetched documents, we must be resuming a
        // migration.
        if (auto topOfOplogBuffer = _donorOplogBuffer->lastObjectPushed(opCtx.get())) {
            startFetchOpTime = uassertStatusOK(OpTime::parseFromOplogEntry(topOfOplogBuffer.get()));
            resumingFromOplogBuffer = true;
        }
    }

    const auto donorMajorityOpTime = _getDonorMajorityOpTime(_oplogFetcherClient);
    if (donorMajorityOpTime < startFetchOpTime) {
        LOGV2_ERROR(5535800,
                    "Donor sync source's majority OpTime is behind our startFetchOpTime",
                    "migrationId"_attr = getMigrationUUID(),
                    "tenantId"_attr = getTenantId(),
                    "donorMajorityOpTime"_attr = donorMajorityOpTime,
                    "startFetchOpTime"_attr = startFetchOpTime);
        const auto now = getGlobalServiceContext()->getFastClockSource()->now();

        stdx::lock_guard lk(_mutex);
        _excludeDonorHost(lk,
                          _oplogFetcherClient->getServerHostAndPort(),
                          now + Milliseconds(tenantMigrationExcludeDonorHostTimeoutMS));
        uasserted(ErrorCodes::InvalidSyncSource,
                  "Donor sync source's majority OpTime is behind our startFetchOpTime, retrying "
                  "sync source selection");
    }

    stdx::unique_lock lk(_mutex);
    OplogFetcher::Config oplogFetcherConfig(
        startFetchOpTime,
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

    // Fetch all oplog entries for shard merge protocol.
    if (_stateDoc.getProtocol() != MigrationProtocolEnum::kShardMerge) {
        oplogFetcherConfig.queryFilter = _getOplogFetcherFilter();
    }

    oplogFetcherConfig.queryReadConcern =
        ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);
    oplogFetcherConfig.requestResumeToken = true;
    oplogFetcherConfig.name =
        "TenantOplogFetcher_" + getTenantId() + "_" + getMigrationUUID().toString();
    oplogFetcherConfig.startingPoint = resumingFromOplogBuffer
        ? OplogFetcher::StartingPoint::kSkipFirstDoc
        : OplogFetcher::StartingPoint::kEnqueueFirstDoc;

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

    lk.unlock();
    _stopOrHangOnFailPoint(&fpAfterStartingOplogFetcherMigrationRecipientInstance);
}

Status TenantMigrationRecipientService::Instance::_enqueueDocuments(
    OplogFetcher::Documents::const_iterator begin,
    OplogFetcher::Documents::const_iterator end,
    const OplogFetcher::DocumentsInfo& info) {

    invariant(_donorOplogBuffer);

    auto opCtx = cc().makeOperationContext();
    if (info.toApplyDocumentCount != 0) {
        // Wait for enough space.
        _donorOplogBuffer->waitForSpace(opCtx.get(), info.toApplyDocumentBytes);

        // Buffer docs for later application.
        _donorOplogBuffer->push(opCtx.get(), begin, end);
    }
    if (info.resumeToken.isNull()) {
        return Status(ErrorCodes::Error(5124600), "Resume token returned is null");
    }

    const auto lastPushedTS = _donorOplogBuffer->getLastPushedTimestamp();
    if (lastPushedTS == info.resumeToken) {
        // We don't want to insert a resume token noop if it would be a duplicate.
        return Status::OK();
    }
    invariant(lastPushedTS < info.resumeToken,
              str::stream() << "LastPushed: " << lastPushedTS.toString()
                            << ", resumeToken: " << info.resumeToken.toString());

    MutableOplogEntry noopEntry;
    noopEntry.setOpType(repl::OpTypeEnum::kNoop);
    noopEntry.setObject(BSON("msg" << TenantMigrationRecipientService::kNoopMsg << "tenantId"
                                   << getTenantId() << "migrationId" << getMigrationUUID()));
    noopEntry.setTimestamp(info.resumeToken);
    // This term is not used for anything.
    noopEntry.setTerm(OpTime::kUninitializedTerm);

    // Use an empty namespace string so this op is ignored by the applier.
    noopEntry.setNss({});
    // Use an empty wall clock time since we have no wall clock time, but we must give it one, and
    // we want it to be clearly fake.
    noopEntry.setWallClockTime({});

    OplogBuffer::Batch noopVec = {noopEntry.toBSON()};
    _donorOplogBuffer->push(opCtx.get(), noopVec.cbegin(), noopVec.cend());
    return Status::OK();
}

void TenantMigrationRecipientService::Instance::_oplogFetcherCallback(Status oplogFetcherStatus) {
    // The oplog fetcher is normally canceled when migration is done; any other error
    // indicates failure.
    if (oplogFetcherStatus.isOK()) {
        // Oplog fetcher status of "OK" means the stopReplProducer failpoint is set.  Migration
        // cannot continue in this state so force a failure.
        LOGV2_ERROR(
            4881205,
            "Recipient migration service oplog fetcher stopped due to stopReplProducer failpoint",
            "tenantId"_attr = getTenantId(),
            "migrationId"_attr = getMigrationUUID());
        _interrupt({ErrorCodes::Error(4881206),
                    "Recipient migration service oplog fetcher stopped due to stopReplProducer "
                    "failpoint"},
                   /*skipWaitingForForgetMigration=*/false);
    } else if (oplogFetcherStatus.code() != ErrorCodes::CallbackCanceled) {
        LOGV2_ERROR(4881204,
                    "Recipient migration service oplog fetcher failed",
                    "tenantId"_attr = getTenantId(),
                    "migrationId"_attr = getMigrationUUID(),
                    "error"_attr = oplogFetcherStatus);
        if (isRetriableOplogFetcherError(oplogFetcherStatus)) {
            LOGV2_DEBUG(5535500,
                        1,
                        "Recipient migration service oplog fetcher received retriable error, "
                        "excluding donor host as sync source and retrying",
                        "tenantId"_attr = getTenantId(),
                        "migrationId"_attr = getMigrationUUID(),
                        "error"_attr = oplogFetcherStatus);

            stdx::lock_guard lk(_mutex);
            const auto now = getGlobalServiceContext()->getFastClockSource()->now();
            _excludeDonorHost(lk,
                              _client->getServerHostAndPort(),
                              now + Milliseconds(tenantMigrationExcludeDonorHostTimeoutMS));
        }
        _interrupt(oplogFetcherStatus, /*skipWaitingForForgetMigration=*/false);
    }
}

void TenantMigrationRecipientService::Instance::_stopOrHangOnFailPoint(FailPoint* fp,
                                                                       OperationContext* opCtx) {
    auto shouldHang = false;
    fp->executeIf(
        [&](const BSONObj& data) {
            LOGV2(4881103,
                  "Tenant migration recipient instance: failpoint enabled",
                  "tenantId"_attr = getTenantId(),
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

OpTime TenantMigrationRecipientService::Instance::_getOplogResumeApplyingDonorOptime(
    const OpTime& cloneFinishedRecipientOpTime) const {
    auto opCtx = cc().makeOperationContext();
    OplogInterfaceLocal oplog(opCtx.get());
    auto oplogIter = oplog.makeIterator();
    auto result = oplogIter->next();

    while (result.isOK()) {
        const auto oplogObj = result.getValue().first;
        auto swRecipientOpTime = repl::OpTime::parseFromOplogEntry(oplogObj);
        uassert(5272311,
                str::stream() << "Unable to parse opTime from oplog entry: " << redact(oplogObj)
                              << ", error: " << swRecipientOpTime.getStatus(),
                swRecipientOpTime.isOK());
        if (swRecipientOpTime.getValue() <= cloneFinishedRecipientOpTime) {
            break;
        }
        const bool isFromCurrentMigration = oplogObj.hasField("fromTenantMigration") &&
            (uassertStatusOK(UUID::parse(oplogObj.getField("fromTenantMigration"))) ==
             getMigrationUUID());
        // Find the most recent no-op oplog entry from the current migration.
        if (isFromCurrentMigration &&
            (oplogObj.getStringField("op") == OpType_serializer(repl::OpTypeEnum::kNoop)) &&
            oplogObj.hasField("o2")) {
            const auto migratedEntryObj = oplogObj.getObjectField("o2");
            const auto swDonorOpTime = repl::OpTime::parseFromOplogEntry(migratedEntryObj);
            uassert(5272305,
                    str::stream() << "Unable to parse opTime from tenant migration oplog entry: "
                                  << redact(oplogObj) << ", error: " << swDonorOpTime.getStatus(),
                    swDonorOpTime.isOK());
            return swDonorOpTime.getValue();
        }
        result = oplogIter->next();
    }
    return OpTime();
}

Future<void> TenantMigrationRecipientService::Instance::_startTenantAllDatabaseCloner(WithLock lk) {
    // If the state is data consistent, do not start the cloner.
    if (_sharedData->getResumePhase() == ResumePhase::kOplogCatchup) {
        return {Future<void>::makeReady()};
    }

    _tenantAllDatabaseCloner = std::make_unique<TenantAllDatabaseCloner>(
        _sharedData.get(),
        _client->getServerHostAndPort(),
        _client.get(),
        repl::StorageInterface::get(cc().getServiceContext()),
        _writerPool.get(),
        _tenantId);
    LOGV2_DEBUG(4881100,
                1,
                "Starting TenantAllDatabaseCloner",
                "migrationId"_attr = getMigrationUUID(),
                "tenantId"_attr = getTenantId());

    auto [startClonerFuture, startCloner] =
        _tenantAllDatabaseCloner->runOnExecutorEvent((**_scopedExecutor).get());

    // runOnExecutorEvent ensures the future is not ready unless an error has occurred.
    if (startClonerFuture.isReady()) {
        auto status = startClonerFuture.getNoThrow();
        uassertStatusOK(status);
        MONGO_UNREACHABLE;
    }

    // Signal the cloner to start.
    (*_scopedExecutor)->signalEvent(startCloner);
    return std::move(startClonerFuture);
}

SemiFuture<void>
TenantMigrationRecipientService::Instance::_advanceMajorityCommitTsToBkpCursorCheckpointTs(
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
        6114000,
        "Advancing recipient's majority commit timestamp to be at least the donor's backup cursor "
        "checkpoint timestamp",
        "migrationId"_attr = getMigrationUUID(),
        "tenantId"_attr = getTenantId(),
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

SemiFuture<void> TenantMigrationRecipientService::Instance::_onCloneSuccess() {
    _stopOrHangOnFailPoint(&fpBeforeMarkingCloneSuccess);
    stdx::lock_guard lk(_mutex);
    // PrimaryOnlyService::onStepUp() before starting instance makes sure that the state doc
    // is majority committed, so we can also skip waiting for it to be majority replicated.
    if (_sharedData->getResumePhase() == ResumePhase::kOplogCatchup) {
        return SemiFuture<void>::makeReady();
    }

    if (_protocol == MigrationProtocolEnum::kMultitenantMigrations) {
        stdx::lock_guard<TenantMigrationSharedData> sharedDatalk(*_sharedData);
        auto lastVisibleMajorityCommittedDonorOpTime =
            _sharedData->getLastVisibleOpTime(sharedDatalk);
        invariant(!lastVisibleMajorityCommittedDonorOpTime.isNull());
        _stateDoc.setDataConsistentStopDonorOpTime(lastVisibleMajorityCommittedDonorOpTime);
    }

    _stateDoc.setCloneFinishedRecipientOpTime(
        repl::ReplicationCoordinator::get(cc().getServiceContext())->getMyLastAppliedOpTime());
    return _updateStateDocForMajority(lk);
}

SemiFuture<TenantOplogApplier::OpTimePair>
TenantMigrationRecipientService::Instance::_waitForDataToBecomeConsistent() {
    stdx::lock_guard lk(_mutex);
    // PrimaryOnlyService::onStepUp() before starting instance makes sure that the state doc
    // is majority committed, so we can also skip waiting for it to be majority replicated.
    if (_stateDoc.getState() == TenantMigrationRecipientStateEnum::kConsistent) {
        return SemiFuture<TenantOplogApplier::OpTimePair>::makeReady(
            TenantOplogApplier::OpTimePair());
    }

    return _tenantOplogApplier->getNotificationForOpTime(
        _stateDoc.getDataConsistentStopDonorOpTime().get());
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_persistConsistentState() {
    stdx::lock_guard lk(_mutex);
    // PrimaryOnlyService::onStepUp() before starting instance makes sure that the state doc
    // is majority committed, so we can also skip waiting for it to be majority replicated.
    if (_stateDoc.getState() == TenantMigrationRecipientStateEnum::kConsistent) {
        return SemiFuture<void>::makeReady();
    }

    // Persist the state that tenant migration instance has reached
    // consistent state.
    _stateDoc.setState(TenantMigrationRecipientStateEnum::kConsistent);
    return _updateStateDocForMajority(lk);
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_enterConsistentState() {
    return _persistConsistentState()
        .thenRunOn(**_scopedExecutor)
        .then([this, self = shared_from_this()]() {
            _stopOrHangOnFailPoint(&fpBeforeFulfillingDataConsistentPromise);
            stdx::lock_guard lk(_mutex);

            auto donorConsistentOpTime = [&]() {
                switch (_protocol) {
                    case MigrationProtocolEnum::kMultitenantMigrations:
                        return _stateDoc.getDataConsistentStopDonorOpTime();
                    case MigrationProtocolEnum::kShardMerge:
                        return _stateDoc.getStartApplyingDonorOpTime();
                    default:
                        MONGO_UNREACHABLE;
                }
                boost::optional<repl::OpTime>();
            }();
            invariant(donorConsistentOpTime && !donorConsistentOpTime->isNull());

            LOGV2_DEBUG(4881101,
                        1,
                        "Tenant migration recipient instance is in consistent state",
                        "migrationId"_attr = getMigrationUUID(),
                        "tenantId"_attr = getTenantId(),
                        "donorConsistentOpTime"_attr = *donorConsistentOpTime);
            if (!_dataConsistentPromise.getFuture().isReady()) {
                _dataConsistentPromise.emplaceValue(*donorConsistentOpTime);
            }
        })
        .semi();
}

namespace {
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

}  // namespace

void TenantMigrationRecipientService::Instance::onMemberImportedFiles(
    const HostAndPort& host, bool success, const boost::optional<StringData>& reason) {
    stdx::lock_guard lk(_mutex);
    if (!_waitingForMembersToImportFiles) {
        LOGV2_WARNING(8423343,
                      "Ignoring delayed recipientVoteImportedFiles",
                      "host"_attr = host.toString(),
                      "migrationId"_attr = _migrationUuid);
        return;
    }

    auto state = _stateDoc.getState();
    uassert(8423341,
            "The migration is at the wrong stage for recipientVoteImportedFiles: {}"_format(
                TenantMigrationRecipientState_serializer(state)),
            state == TenantMigrationRecipientStateEnum::kLearnedFilenames);

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
        repl::ReplicationCoordinator::get(getGlobalServiceContext())
            ->getConfig()
            .getNumDataBearingMembers()) {
        LOGV2_INFO(6112809,
                   "All members finished importing donated files",
                   "migrationId"_attr = _migrationUuid);
        _importedFilesPromise.emplaceValue();
        _waitingForMembersToImportFiles = false;
    }
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_markStateDocAsGarbageCollectable() {
    _stopOrHangOnFailPoint(&fpAfterReceivingRecipientForgetMigration);

    // Throws if we have failed to persist the state doc at the first place. This can only happen in
    // unittests where we enable the autoRecipientForgetMigration failpoint. Otherwise,
    // recipientForgetMigration will always wait for the state doc to be persisted first and thus
    // this will only be called with _stateDocPersistedPromise resolved OK.
    invariant(_stateDocPersistedPromise.getFuture().isReady());
    _stateDocPersistedPromise.getFuture().get();

    stdx::lock_guard lk(_mutex);

    if (_stateDoc.getExpireAt()) {
        // Nothing to do if the state doc already has the expireAt set.
        return SemiFuture<void>::makeReady();
    }

    _stateDoc.setState(TenantMigrationRecipientStateEnum::kDone);
    _stateDoc.setExpireAt(getGlobalServiceContext()->getFastClockSource()->now() +
                          Milliseconds{repl::tenantMigrationGarbageCollectionDelayMS.load()});

    return ExecutorFuture(**_scopedExecutor)
        .then([this, self = shared_from_this(), stateDoc = _stateDoc] {
            auto opCtx = cc().makeOperationContext();
            auto status = [&]() {
                try {
                    // Update the state doc with the expireAt set.
                    return tenantMigrationRecipientEntryHelpers::updateStateDoc(opCtx.get(),
                                                                                stateDoc);
                } catch (DBException& ex) {
                    return ex.toStatus();
                }
            }();

            if (!status.isOK()) {
                // We assume that we only fail with shutDown/stepDown errors (i.e. for failovers).
                // Otherwise, the whole chain would stop running without marking the state doc
                // garbage collectable while we are still the primary.
                invariant(ErrorCodes::isShutdownError(status) ||
                          ErrorCodes::isNotPrimaryError(status));
                uassertStatusOK(status);
            }

            auto writeOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            return WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(writeOpTime, CancellationToken::uncancelable());
        })
        .semi();
}

void TenantMigrationRecipientService::Instance::_cancelRemainingWork(WithLock lk) {
    if (_sharedData) {
        stdx::lock_guard<TenantMigrationSharedData> sharedDatalk(*_sharedData);
        // Prevents the tenant cloner from getting retried on retryable errors.
        _sharedData->setStatusIfOK(
            sharedDatalk, Status{ErrorCodes::CallbackCanceled, "Tenant migration cloner canceled"});
    }

    if (_client) {
        // interrupts running tenant cloner.
        _client->shutdownAndDisallowReconnect();
    }

    if (_oplogFetcherClient) {
        // interrupts running tenant oplog fetcher.
        _oplogFetcherClient->shutdownAndDisallowReconnect();
    }

    // Interrupts running oplog applier.
    shutdownTarget(lk, _tenantOplogApplier);
}

void TenantMigrationRecipientService::Instance::_interrupt(Status status,
                                                           bool skipWaitingForForgetMigration) {
    invariant(!status.isOK());

    stdx::lock_guard lk(_mutex);

    if (skipWaitingForForgetMigration) {
        // We only get here on stepDown/shutDown.
        setPromiseErrorifNotReady(lk, _receivedRecipientForgetMigrationPromise, status);
    }

    if (_taskState.isInterrupted() || _taskState.isDone()) {
        // nothing to do.
        return;
    }

    _cancelRemainingWork(lk);

    if (_donorFilenameBackupCursorFileFetcher) {
        _donorFilenameBackupCursorFileFetcher->shutdown();
    }

    // If the task is running, then setting promise result will be taken care by the main task
    // continuation chain.
    if (_taskState.isNotStarted()) {
        invariant(skipWaitingForForgetMigration);
        _stateDocPersistedPromise.setError(status);
        _dataConsistentPromise.setError(status);
        _dataSyncCompletionPromise.setError(status);

        // The interrupt() is called before the instance is scheduled to run. If the state doc has
        // already been marked garbage collectable, resolve the completion promise with OK.
        if (_stateDoc.getExpireAt()) {
            _taskCompletionPromise.emplaceValue();
        } else {
            _taskCompletionPromise.setError(status);
        }
    }

    _taskState.setState(TaskState::kInterrupted, status);
}

void TenantMigrationRecipientService::Instance::interrupt(Status status) {
    _interrupt(status, /*skipWaitingForForgetMigration=*/true);
}

void TenantMigrationRecipientService::Instance::cancelMigration() {
    auto status = Status(ErrorCodes::TenantMigrationAborted,
                         "Interrupted all tenant migrations on recipient");
    _interrupt(status, /*skipWaitingForForgetMigration=*/false);
}

void TenantMigrationRecipientService::Instance::onReceiveRecipientForgetMigration(
    OperationContext* opCtx) {
    LOGV2(4881400,
          "Forgetting migration due to recipientForgetMigration command",
          "migrationId"_attr = getMigrationUUID(),
          "tenantId"_attr = getTenantId());

    // Wait until the state doc is initialized and persisted.
    _stateDocPersistedPromise.getFuture().get(opCtx);

    {
        stdx::lock_guard lk(_mutex);
        if (_receivedRecipientForgetMigrationPromise.getFuture().isReady()) {
            return;
        }
        _receivedRecipientForgetMigrationPromise.emplaceValue();
    }

    // Interrupt the chain to mark the state doc garbage collectable.
    _interrupt(Status(ErrorCodes::TenantMigrationForgotten,
                      str::stream() << "recipientForgetMigration received for migration "
                                    << getMigrationUUID()),
               /*skipWaitingForForgetMigration=*/false);
}

void TenantMigrationRecipientService::Instance::_cleanupOnDataSyncCompletion(Status status) {
    auto opCtx = cc().makeOperationContext();

    std::unique_ptr<OplogFetcher> savedDonorOplogFetcher;
    std::shared_ptr<TenantOplogApplier> savedTenantOplogApplier;
    std::unique_ptr<ThreadPool> savedWriterPool;
    std::unique_ptr<Fetcher> savedDonorFilenameBackupCursorFileFetcher;
    {
        stdx::lock_guard lk(_mutex);
        _cancelRemainingWork(lk);

        _backupCursorKeepAliveCancellation = {};
        _backupCursorKeepAliveFuture = boost::none;

        if (_donorFilenameBackupCursorFileFetcher) {
            shutdownTarget(lk, _donorFilenameBackupCursorFileFetcher);
        }
        shutdownTarget(lk, _donorOplogFetcher);
        shutdownTargetWithOpCtx(lk, _donorOplogBuffer, opCtx.get());

        _donorReplicaSetMonitor = nullptr;

        invariant(!status.isOK());
        setPromiseErrorifNotReady(lk, _stateDocPersistedPromise, status);
        setPromiseErrorifNotReady(lk, _dataConsistentPromise, status);
        setPromiseErrorifNotReady(lk, _dataSyncCompletionPromise, status);

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

BSONObj TenantMigrationRecipientService::Instance::_getOplogFetcherFilter() const {
    // Either the namespace belongs to the tenant, or it's an applyOps in the admin namespace
    // and the first operation belongs to the tenant.  A transaction with mixed tenant/non-tenant
    // operations should not be possible and will fail in the TenantOplogApplier.
    //
    // Commit of prepared transactions is not handled here; we'd need to handle them in the applier
    // by allowing all commits through here and ignoring those not corresponding to active
    // transactions.
    BSONObj namespaceRegex = ClonerUtils::makeTenantDatabaseRegex(getTenantId());
    return BSON("$or" << BSON_ARRAY(BSON("ns" << namespaceRegex)
                                    << BSON("ns"
                                            << "admin.$cmd"
                                            << "o.applyOps.0.ns" << namespaceRegex)));
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_updateStateDocForMajority(
    WithLock lk) const {
    return ExecutorFuture(**_scopedExecutor)
        .then([this, self = shared_from_this(), stateDoc = _stateDoc] {
            auto opCtx = cc().makeOperationContext();
            uassertStatusOK(
                tenantMigrationRecipientEntryHelpers::updateStateDoc(opCtx.get(), stateDoc));

            auto writeOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            return WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(writeOpTime, CancellationToken::uncancelable());
        })
        .semi();
}

void TenantMigrationRecipientService::Instance::_fetchAndStoreDonorClusterTimeKeyDocs(
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

SemiFuture<void>
TenantMigrationRecipientService::Instance::_checkIfFcvHasChangedSinceLastAttempt() {
    stdx::lock_guard lk(_mutex);

    // Record the FCV at the start of a migration and check for changes in every
    // subsequent attempt. Fail if there is any mismatch in FCV or
    // upgrade/downgrade state. (Generic FCV reference): This FCV check should
    // exist across LTS binary versions.
    auto currentFCV = serverGlobalParams.featureCompatibility.getVersion();
    auto startingFCV = _stateDoc.getRecipientPrimaryStartingFCV();

    if (!startingFCV) {
        _stateDoc.setRecipientPrimaryStartingFCV(currentFCV);
        return _updateStateDocForMajority(lk);
    }

    if (startingFCV != currentFCV) {
        LOGV2_ERROR(5356200,
                    "FCV may not change during migration",
                    "tenantId"_attr = getTenantId(),
                    "migrationId"_attr = getMigrationUUID(),
                    "startingFCV"_attr = startingFCV,
                    "currentFCV"_attr = currentFCV);
        uasserted(5356201, "Detected FCV change from last migration attempt.");
    }

    return SemiFuture<void>::makeReady();
}

void TenantMigrationRecipientService::Instance::_compareRecipientAndDonorFCV() const {
    if (skipComparingRecipientAndDonorFCV.shouldFail()) {  // Test-only.
        return;
    }
    FindCommandRequest findCmd{NamespaceString::kServerConfigurationNamespace};
    findCmd.setFilter(BSON("_id" << multiversion::kParameterName));
    findCmd.setReadConcern(ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    auto donorFCVbson = _client->findOne(std::move(findCmd),
                                         ReadPreferenceSetting{ReadPreference::SecondaryPreferred});

    uassert(5382302, "FCV on donor not set", !donorFCVbson.isEmpty());

    auto swDonorFCV = FeatureCompatibilityVersionParser::parse(donorFCVbson);
    uassertStatusOK(swDonorFCV.getStatus());

    stdx::lock_guard lk(_mutex);
    auto donorFCV = swDonorFCV.getValue();
    auto recipientFCV = _stateDoc.getRecipientPrimaryStartingFCV();

    if (donorFCV != recipientFCV) {
        LOGV2_ERROR(5382300,
                    "Donor and recipient FCV mismatch",
                    "tenantId"_attr = getTenantId(),
                    "migrationId"_attr = getMigrationUUID(),
                    "donorConnString"_attr = _donorConnectionString,
                    "donorFCV"_attr = donorFCV,
                    "recipientFCV"_attr = recipientFCV);
        uasserted(5382301, "Mismatch between donor and recipient FCV");
    }
}

bool TenantMigrationRecipientService::Instance::_checkifProtocolRemainsFCVCompatible() {
    stdx::lock_guard<Latch> lg(_mutex);

    // Ensure that the on-disk protocol and cached value remains the same.
    invariant(!_stateDoc.getProtocol() || _stateDoc.getProtocol().value() == getProtocol());

    auto isAtLeastFCV52AtStart = serverGlobalParams.featureCompatibility.isGreaterThanOrEqualTo(
        multiversion::FeatureCompatibilityVersion::kVersion_5_2);
    if (isAtLeastFCV52AtStart) {
        // When the instance is started using state doc < 5.2 FCV format, _stateDoc._protocol field
        // won't be set. In that case, the cached value Instance::_protocol will be set to
        // "kMultitenantMigrations".
        return true;
    }

    if (getProtocol() == MigrationProtocolEnum::kShardMerge) {
        LOGV2(5949504,
              "Must abort tenant migration as 'Merge' protocol is not supported for FCV "
              "below 5.2");
        return false;
    }
    // For backward compatibility, ensure that the 'protocol' field is not set in the
    // document.
    _stateDoc.setProtocol(boost::none);
    return true;
}

void TenantMigrationRecipientService::Instance::_startOplogApplier() {
    _stopOrHangOnFailPoint(&fpAfterFetchingCommittedTransactions);

    stdx::unique_lock lk(_mutex);
    const auto& cloneFinishedRecipientOpTime = _stateDoc.getCloneFinishedRecipientOpTime();
    invariant(cloneFinishedRecipientOpTime);

    OpTime resumeOpTime;
    if (_sharedData->getResumePhase() == ResumePhase::kOplogCatchup) {
        lk.unlock();
        // We avoid holding the mutex while scanning the local oplog which
        // acquires the RSTL in IX mode. This is to allow us to be interruptable
        // via a concurrent stepDown which acquires the RSTL in X mode.
        resumeOpTime = _getOplogResumeApplyingDonorOptime(*cloneFinishedRecipientOpTime);
        lk.lock();
    }

    // Throwing error when cloner is canceled externally via interrupt(),
    // makes the instance to skip the remaining task (i.e., starting oplog
    // applier) in the sync process. This step is necessary to prevent race
    // between interrupt() and starting oplog applier for the failover
    // scenarios where we don't start the cloner if the tenant data is
    // already in consistent state.
    if (_taskState.isInterrupted()) {
        uassertStatusOK(_taskState.getInterruptStatus());
    }

    const auto& startApplyingDonorOpTime = _stateDoc.getStartApplyingDonorOpTime();
    invariant(startApplyingDonorOpTime);

    _tenantOplogApplier = std::make_shared<TenantOplogApplier>(
        _migrationUuid,
        _protocol,
        (_protocol != MigrationProtocolEnum::kShardMerge) ? boost::make_optional(_tenantId)
                                                          : boost::none,
        (!resumeOpTime.isNull()) ? std::max(resumeOpTime, *startApplyingDonorOpTime)
                                 : *startApplyingDonorOpTime,
        _donorOplogBuffer.get(),
        **_scopedExecutor,
        _writerPool.get(),
        resumeOpTime.getTimestamp());
    _tenantOplogApplier->setCloneFinishedRecipientOpTime(*cloneFinishedRecipientOpTime);

    LOGV2_DEBUG(4881202,
                1,
                "Recipient migration service starting oplog applier",
                "tenantId"_attr = getTenantId(),
                "migrationId"_attr = getMigrationUUID(),
                "startApplyingAfterDonorOpTime"_attr =
                    _tenantOplogApplier->getStartApplyingAfterOpTime(),
                "resumeBatchingTs"_attr = _tenantOplogApplier->getResumeBatchingTs());

    uassertStatusOK(_tenantOplogApplier->startup());
    _oplogApplierReady = true;
    _oplogApplierReadyCondVar.notify_all();

    lk.unlock();
    _stopOrHangOnFailPoint(&fpAfterStartingOplogApplierMigrationRecipientInstance);
}

void TenantMigrationRecipientService::Instance::_setup() {
    auto uniqueOpCtx = cc().makeOperationContext();
    auto opCtx = uniqueOpCtx.get();
    {
        stdx::lock_guard lk(_mutex);
        // Do not set the internal states if the migration is already interrupted.
        if (_taskState.isInterrupted()) {
            uassertStatusOK(_taskState.getInterruptStatus());
        }

        // Reuse the _writerPool for retry of the future chain.
        if (!_writerPool) {
            _writerPool = makeTenantMigrationWriterPool();
        }

        ResumePhase resumePhase = [&] {
            if (_stateDoc.getCloneFinishedRecipientOpTime()) {
                invariant(_stateDoc.getStartFetchingDonorOpTime());
                return ResumePhase::kOplogCatchup;
            }
            if (_stateDoc.getStartFetchingDonorOpTime()) {
                return ResumePhase::kDataSync;
            }
            return ResumePhase::kNone;
        }();

        _sharedData = std::make_unique<TenantMigrationSharedData>(
            getGlobalServiceContext()->getFastClockSource(), getMigrationUUID(), resumePhase);

        _createOplogBuffer(lk, opCtx);
    }

    // Start the oplog buffer outside the mutex to avoid deadlock on a concurrent stepdown.
    try {
        // It is illegal to start the replicated donor buffer when the node is not primary.
        // So ensure we are primary before trying to startup the oplog buffer.
        repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);

        auto oplogBufferNS = getOplogBufferNs(getMigrationUUID());
        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(
                opCtx, oplogBufferNS.db())) {
            uassertStatusOK(
                Status(ErrorCodes::NotWritablePrimary, "Recipient node is no longer a primary."));
        }

        _donorOplogBuffer->startup(opCtx);
    } catch (DBException& ex) {
        ex.addContext("Failed to create oplog buffer collection.");
        throw;
    }
}

SemiFuture<TenantOplogApplier::OpTimePair>
TenantMigrationRecipientService::Instance::_waitForOplogApplierToStop() {
    _stopOrHangOnFailPoint(&fpAfterDataConsistentMigrationRecipientInstance);

    stdx::lock_guard lk(_mutex);
    // wait for oplog applier to complete/stop.
    // The oplog applier does not exit normally; it must be shut down externally,
    // e.g. by recipientForgetMigration.
    return _tenantOplogApplier->getNotificationForOpTime(OpTime::max());
}

SemiFuture<TenantOplogApplier::OpTimePair>
TenantMigrationRecipientService::Instance::_migrateUsingMTMProtocol(
    const CancellationToken& token) {
    return ExecutorFuture(**_scopedExecutor)
        .then([this, self = shared_from_this()] { return _getStartOpTimesFromDonor(); })
        .then([this, self = shared_from_this()] {
            return _fetchRetryableWritesOplogBeforeStartOpTime();
        })
        .then([this, self = shared_from_this()] { _startOplogFetcher(); })
        .then([this, self = shared_from_this()] {
            stdx::lock_guard lk(_mutex);
            return _startTenantAllDatabaseCloner(lk);
        })
        .then([this, self = shared_from_this()] { return _onCloneSuccess(); })
        .then([this, self = shared_from_this()] {
            return _fetchCommittedTransactionsBeforeStartOpTime();
        })
        .then([this, self = shared_from_this()] { return _startOplogApplier(); })
        .then([this, self = shared_from_this()] {
            _stopOrHangOnFailPoint(&fpAfterStartingOplogApplierMigrationRecipientInstance);
            return _waitForDataToBecomeConsistent();
        })
        .then(
            [this, self = shared_from_this()](TenantOplogApplier::OpTimePair donorRecipientOpTime) {
                return _enterConsistentState();
            })
        .then([this, self = shared_from_this()] { return _waitForOplogApplierToStop(); })
        .semi();
}

SemiFuture<TenantOplogApplier::OpTimePair>
TenantMigrationRecipientService::Instance::_migrateUsingShardMergeProtocol(
    const CancellationToken& token) {
    return ExecutorFuture(**_scopedExecutor)
        .then(
            [this, self = shared_from_this(), token] { return _openBackupCursorWithRetry(token); })
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
        .then([this, self = shared_from_this()] {
            LOGV2_INFO(6113402, "Waiting for all nodes to call recipientVoteImportedFiles");
            return _importedFilesPromise.getFuture().semi();
        })
        .then([this, self = shared_from_this()] { return _killBackupCursor(); })
        .then([this, self = shared_from_this()] { return _onCloneSuccess(); })
        .then([this, self = shared_from_this()] { return _enterConsistentState(); })
        .then([this, self = shared_from_this()] {
            return _fetchCommittedTransactionsBeforeStartOpTime();
        })
        .then([this, self = shared_from_this()] { return _startOplogApplier(); })
        .then([this, self = shared_from_this()] { return _waitForOplogApplierToStop(); })
        .semi();
}

SemiFuture<void> TenantMigrationRecipientService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    _scopedExecutor = executor;
    auto scopedOutstandingMigrationCounter =
        TenantMigrationStatistics::get(_serviceContext)->getScopedOutstandingReceivingCount();

    LOGV2(4879607,
          "Starting tenant migration recipient instance: ",
          "migrationId"_attr = getMigrationUUID(),
          "protocol"_attr = getProtocol(),
          "tenantId"_attr = getTenantId(),
          "connectionString"_attr = _donorConnectionString,
          "readPreference"_attr = _readPreference);

    pauseBeforeRunTenantMigrationRecipientInstance.pauseWhileSet();

    bool cancelWhenDurable = false;
    auto isFCVUpgradingOrDowngrading = [&]() -> bool {
        // We must abort the migration if we try to start or resume while upgrading or downgrading.
        // We defer this until after the state doc is persisted in a started so as to make sure it
        // it safe to abort and forget the migration. (Generic FCV reference): This FCV check should
        // exist across LTS binary versions.
        if (serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading()) {
            LOGV2(5356304, "Must abort tenant migration as recipient is upgrading or downgrading");
            return true;
        }
        return false;
    };

    // Tenant migrations gets aborted on FCV upgrading or downgrading state. But,
    // due to race between between Instance::getOrCreate() and
    // SetFeatureCompatibilityVersionCommand::_cancelTenantMigrations(), we might miss aborting this
    // tenant migration and FCV might have updated or downgraded at this point. So, need to ensure
    // that the protocol is still compatible with FCV.
    if (isFCVUpgradingOrDowngrading() || !_checkifProtocolRemainsFCVCompatible()) {
        cancelWhenDurable = true;
    }

    // Any FCV changes after this point will abort this migration.
    //
    // The 'AsyncTry' is run on the cleanup executor as opposed to the scoped executor as we rely
    // on the 'PrimaryService' to interrupt the operation contexts based on thread pool and not the
    // executor.
    return AsyncTry([this, self = shared_from_this(), executor, token, cancelWhenDurable] {
               return ExecutorFuture(**executor)
                   .then([this, self = shared_from_this()] {
                       auto opCtx = cc().makeOperationContext();

                       stdx::unique_lock lk(_mutex);
                       // Instance task can be started only once for the current term on a primary.
                       invariant(!_taskState.isDone());
                       // If the task state is interrupted, then don't start the task.
                       if (_taskState.isInterrupted()) {
                           uassertStatusOK(_taskState.getInterruptStatus());
                       }

                       // The task state will already have been set to 'kRunning' if we restarted
                       // the future chain on donor failover.
                       if (!_taskState.isRunning()) {
                           _taskState.setState(TaskState::kRunning);
                       }
                       pauseAfterRunTenantMigrationRecipientInstance.pauseWhileSet();

                       auto mtab = tenant_migration_access_blocker::
                           getTenantMigrationRecipientAccessBlocker(_serviceContext,
                                                                    _stateDoc.getTenantId());
                       if (mtab && mtab->getMigrationId() != _migrationUuid) {
                           // There is a conflicting migration. If its state doc has already been
                           // marked as garbage collectable, this instance must correspond to a
                           // retry and we can delete immediately to allow the migration to restart.
                           // Otherwise, there is a real conflict so we should throw
                           // ConflictingInProgress.
                           lk.unlock();
                           auto deleted =
                               uassertStatusOK(tenantMigrationRecipientEntryHelpers::
                                                   deleteStateDocIfMarkedAsGarbageCollectable(
                                                       opCtx.get(), _tenantId));
                           uassert(ErrorCodes::ConflictingOperationInProgress,
                                   str::stream()
                                       << "Found active migration for tenantId \"" << _tenantId
                                       << "\" with migration id " << mtab->getMigrationId(),
                                   deleted);
                           lk.lock();
                       }

                       if (_stateDoc.getState() !=
                               TenantMigrationRecipientStateEnum::kUninitialized &&
                           _stateDoc.getState() != TenantMigrationRecipientStateEnum::kDone &&
                           !_stateDocPersistedPromise.getFuture().isReady() &&
                           !_stateDoc.getExpireAt()) {
                           // If our state is initialized and we haven't fulfilled the
                           // '_stateDocPersistedPromise' yet, it means we are restarting the future
                           // chain due to recipient failover.
                           _stateDoc.setNumRestartsDueToRecipientFailure(
                               _stateDoc.getNumRestartsDueToRecipientFailure() + 1);
                           const auto stateDoc = _stateDoc;
                           lk.unlock();
                           // Update the state document outside the mutex to avoid a deadlock in the
                           // case of a concurrent stepdown.
                           uassertStatusOK(tenantMigrationRecipientEntryHelpers::updateStateDoc(
                               opCtx.get(), stateDoc));
                           return SemiFuture<void>::makeReady();
                       }
                       return _initializeStateDoc(lk);
                   })
                   .then([this, self = shared_from_this(), cancelWhenDurable] {
                       if (_stateDocPersistedPromise.getFuture().isReady()) {
                           // This is a retry of the future chain due to donor failure.
                           auto opCtx = cc().makeOperationContext();
                           TenantMigrationRecipientDocument stateDoc;
                           {
                               stdx::lock_guard lk(_mutex);
                               _stateDoc.setNumRestartsDueToDonorConnectionFailure(
                                   _stateDoc.getNumRestartsDueToDonorConnectionFailure() + 1);
                               stateDoc = _stateDoc;
                           }
                           uassertStatusOK(tenantMigrationRecipientEntryHelpers::updateStateDoc(
                               opCtx.get(), stateDoc));
                       } else {
                           // Avoid fulfilling the promise twice on restart of the future chain.
                           _stateDocPersistedPromise.emplaceValue();
                       }

                       {
                           stdx::lock_guard<Latch> lg(_mutex);
                           uassert(ErrorCodes::TenantMigrationForgotten,
                                   str::stream() << "Migration " << getMigrationUUID()
                                                 << " already marked for garbage collection",
                                   _stateDoc.getState() !=
                                           TenantMigrationRecipientStateEnum::kDone &&
                                       !_stateDoc.getExpireAt());
                       }

                       // Must abort if flagged for cancellation above.
                       uassert(ErrorCodes::TenantMigrationAborted,
                               "Interrupted tenant migration on recipient",
                               !cancelWhenDurable);

                       _stopOrHangOnFailPoint(
                           &fpAfterPersistingTenantMigrationRecipientInstanceStateDoc);
                       return _createAndConnectClients();
                   })
                   .then([this, self = shared_from_this(), token] {
                       _stopOrHangOnFailPoint(&fpBeforeFetchingDonorClusterTimeKeys);
                       _fetchAndStoreDonorClusterTimeKeyDocs(token);
                   })
                   .then([this, self = shared_from_this()] {
                       _stopOrHangOnFailPoint(&fpAfterConnectingTenantMigrationRecipientInstance);
                       return _checkIfFcvHasChangedSinceLastAttempt();
                   })
                   .then([this, self = shared_from_this()] {
                       _stopOrHangOnFailPoint(&fpAfterRecordingRecipientPrimaryStartingFCV);
                       _compareRecipientAndDonorFCV();
                   })
                   .then([this, self = shared_from_this()] {
                       _stopOrHangOnFailPoint(&fpAfterComparingRecipientAndDonorFCV);
                       // Sets up internal state to begin migration.
                       _setup();
                   })
                   .then([this, self = shared_from_this(), token] {
                       switch (_protocol) {
                           case MigrationProtocolEnum::kMultitenantMigrations:
                               return _migrateUsingMTMProtocol(token);
                           case MigrationProtocolEnum::kShardMerge:
                               return _migrateUsingShardMergeProtocol(token);
                           default:
                               MONGO_UNREACHABLE;
                       }
                   });
           })
        .until([this, self = shared_from_this()](
                   StatusOrStatusWith<TenantOplogApplier::OpTimePair> applierStatus) {
            hangMigrationBeforeRetryCheck.pauseWhileSet();

            auto shouldRetryMigration = [&](WithLock, Status status) -> bool {
                // Shard merge is not resumable for any replica set state transitions or network
                // errors.
                if (getProtocol() == MigrationProtocolEnum::kShardMerge)
                    return false;

                // We shouldn't retry migration after receiving the recipientForgetMigration command
                // or on stepDown/shutDown.
                if (_receivedRecipientForgetMigrationPromise.getFuture().isReady())
                    return false;

                if (!_stateDocPersistedPromise.getFuture().isReady())
                    return false;

                return ErrorCodes::isRetriableError(status) || isRetriableOplogFetcherError(status);
            };

            stdx::unique_lock lk(_mutex);
            auto status = _taskState.isInterrupted() ? _taskState.getInterruptStatus()
                                                     : applierStatus.getStatus();

            if (shouldRetryMigration(lk, status)) {
                // Reset the task state and clear the interrupt status.
                if (!_taskState.isRunning()) {
                    _taskState.setState(TaskState::kRunning);
                }
                _oplogApplierReady = false;
                // Clean up the async components before retrying the future chain.
                std::unique_ptr<OplogFetcher> savedDonorOplogFetcher;
                std::shared_ptr<TenantOplogApplier> savedTenantOplogApplier;

                _cancelRemainingWork(lk);
                shutdownTarget(lk, _donorOplogFetcher);

                // Swap the oplog fetcher and applier to join() outside of '_mutex'.
                using std::swap;
                swap(savedDonorOplogFetcher, _donorOplogFetcher);
                swap(savedTenantOplogApplier, _tenantOplogApplier);
                lk.unlock();

                // Perform join outside the lock to avoid deadlocks.
                joinTarget(savedDonorOplogFetcher);
                joinTarget(savedTenantOplogApplier);
                return false;
            }
            return true;
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(_recipientService->getInstanceCleanupExecutor(), token)
        .semi()
        .thenRunOn(_recipientService->getInstanceCleanupExecutor())
        .onCompletion([this, self = shared_from_this()](
                          StatusOrStatusWith<TenantOplogApplier::OpTimePair> applierStatus) {
            // On shutDown/stepDown, the _scopedExecutor may have already been shut down. So we
            // need to schedule the clean up work on the parent executor.

            // We don't need the final optime from the oplog applier. The data sync does not
            // normally stop by itself on success. It completes only on errors or on external
            // interruption (e.g. by shutDown/stepDown or by recipientForgetMigration command).
            Status status = applierStatus.getStatus();

            // If we were interrupted during oplog application, replace oplog application
            // status with error state.
            // Network and cancellation errors can be caused due to interrupt() (which shuts
            // down the cloner/fetcher dbClientConnection & oplog applier), so replace those
            // error status with interrupt status, if set.
            if (ErrorCodes::isCancellationError(status) || ErrorCodes::isNetworkError(status)) {
                stdx::lock_guard lk(_mutex);
                if (_taskState.isInterrupted()) {
                    LOGV2(4881207,
                          "Migration completed with both error and interrupt",
                          "tenantId"_attr = getTenantId(),
                          "migrationId"_attr = getMigrationUUID(),
                          "completionStatus"_attr = status,
                          "interruptStatus"_attr = _taskState.getInterruptStatus());
                    status = _taskState.getInterruptStatus();
                } else if (status == ErrorCodes::CallbackCanceled) {
                    // All of our async components don't exit with CallbackCanceled normally unless
                    // they are shut down by the instance itself via interrupt. If we get a
                    // CallbackCanceled error without an interrupt, it is coming from the service's
                    // cancellation token on failovers. It is possible for the token to get canceled
                    // before the instance is interrupted, so we replace the CallbackCanceled error
                    // with InterruptedDueToReplStateChange and treat it as a retryable error.
                    status = Status{ErrorCodes::InterruptedDueToReplStateChange,
                                    "operation was interrupted"};
                }
            }

            LOGV2(4878501,
                  "Tenant migration recipient instance: Data sync completed.",
                  "tenantId"_attr = getTenantId(),
                  "migrationId"_attr = getMigrationUUID(),
                  "error"_attr = status);

            if (MONGO_unlikely(hangBeforeTaskCompletion.shouldFail())) {
                LOGV2(4881102,
                      "Tenant migration recipient instance: hangBeforeTaskCompletion failpoint "
                      "enabled");
                hangBeforeTaskCompletion.pauseWhileSet();
            }

            _cleanupOnDataSyncCompletion(status);
            _setMigrationStatsOnCompletion(status);

            // Handle recipientForgetMigration.
            stdx::lock_guard lk(_mutex);
            if (_stateDoc.getExpireAt() ||
                MONGO_unlikely(autoRecipientForgetMigration.shouldFail())) {
                // Skip waiting for the recipientForgetMigration command.
                setPromiseOkifNotReady(lk, _receivedRecipientForgetMigrationPromise);
            }
        })
        .thenRunOn(**_scopedExecutor)
        .then([this, self = shared_from_this()] {
            // Schedule on the _scopedExecutor to make sure we are still the primary when
            // waiting for the recipientForgetMigration command.
            return _receivedRecipientForgetMigrationPromise.getFuture();
        })
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
            _stopOrHangOnFailPoint(&fpBeforeDroppingOplogBufferCollection);
            auto opCtx = cc().makeOperationContext();
            auto storageInterface = StorageInterface::get(opCtx.get());

            // The oplog buffer collection can be safely dropped at this point. In case it
            // doesn't exist, dropping will be a no-op. It isn't necessary that the drop is
            // majority-committed. A new primary will attempt to drop the collection anyway.
            return storageInterface->dropCollection(opCtx.get(),
                                                    getOplogBufferNs(getMigrationUUID()));
        })
        .thenRunOn(_recipientService->getInstanceCleanupExecutor())
        .onCompletion([this,
                       self = shared_from_this(),
                       scopedCounter{std::move(scopedOutstandingMigrationCounter)}](Status status) {
            // Schedule on the parent executor to mark the completion of the whole chain so this
            // is safe even on shutDown/stepDown.
            stdx::lock_guard lk(_mutex);
            invariant(_dataSyncCompletionPromise.getFuture().isReady());
            if (status.isOK()) {
                LOGV2(4881401,
                      "Migration marked to be garbage collectable due to "
                      "recipientForgetMigration "
                      "command",
                      "migrationId"_attr = getMigrationUUID(),
                      "tenantId"_attr = getTenantId(),
                      "expireAt"_attr = *_stateDoc.getExpireAt());
                setPromiseOkifNotReady(lk, _taskCompletionPromise);
            } else {
                // We should only hit here on a stepDown/shutDown, or a 'conflicting migration'
                // error.
                LOGV2(4881402,
                      "Migration not marked to be garbage collectable",
                      "migrationId"_attr = getMigrationUUID(),
                      "tenantId"_attr = getTenantId(),
                      "status"_attr = status);
                setPromiseErrorifNotReady(lk, _taskCompletionPromise, status);
            }
            _taskState.setState(TaskState::kDone);
        })
        .semi();
}

void TenantMigrationRecipientService::Instance::_setMigrationStatsOnCompletion(
    Status completionStatus) const {
    bool success = false;

    if (completionStatus.code() == ErrorCodes::TenantMigrationForgotten) {
        stdx::lock_guard lk(_mutex);
        if (_stateDoc.getExpireAt()) {
            // Avoid double counting tenant migration statistics after failover.
            return;
        }
        // The migration committed if and only if it received recipientForgetMigration after it has
        // applied data past the returnAfterReachingDonorTimestamp, saved in state doc as
        // rejectReadsBeforeTimestamp.
        if (_stateDoc.getRejectReadsBeforeTimestamp().has_value()) {
            success = true;
        }
    } else if (ErrorCodes::isRetriableError(completionStatus)) {
        // The migration was interrupted due to shutdown or stepdown, avoid incrementing the count
        // for failed migrations since the migration will be resumed on stepup.
        return;
    }

    if (success) {
        TenantMigrationStatistics::get(_serviceContext)->incTotalSuccessfulMigrationsReceived();
    } else {
        TenantMigrationStatistics::get(_serviceContext)->incTotalFailedMigrationsReceived();
    }
}

const UUID& TenantMigrationRecipientService::Instance::getMigrationUUID() const {
    return _migrationUuid;
}

const std::string& TenantMigrationRecipientService::Instance::getTenantId() const {
    return _tenantId;
}

const MigrationProtocolEnum& TenantMigrationRecipientService::Instance::getProtocol() const {
    return _protocol;
}

TenantMigrationRecipientDocument TenantMigrationRecipientService::Instance::getState() const {
    stdx::lock_guard lk(_mutex);
    return _stateDoc;
}

}  // namespace repl
}  // namespace mongo
