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


#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <iterator>
#include <mutex>
#include <set>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/internal_auth.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/keys_collection_util.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/repl/tenant_migration_access_blocker.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/tenant_migration_recipient_entry_helpers.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_migration_statistics.h"
#include "mongo/db/repl/tenant_migration_util.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/serverless/serverless_types_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

namespace mongo {
namespace repl {
namespace {
using namespace fmt;
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());
constexpr StringData kOplogBufferPrefix = "repl.migration.oplog_"_sd;

NamespaceString getOplogBufferNs(const UUID& migrationUUID) {
    return NamespaceString::makeGlobalConfigCollection(kOplogBufferPrefix +
                                                       migrationUUID.toString());
}

bool isMigrationCompleted(TenantMigrationRecipientStateEnum state) {
    return state == TenantMigrationRecipientStateEnum::kDone ||
        state == TenantMigrationRecipientStateEnum::kAborted ||
        state == TenantMigrationRecipientStateEnum::kCommitted;
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

// Creates the oplog buffer that will be populated by donor oplog entries from the retryable
// writes fetching stage and oplog fetching stage.
std::shared_ptr<OplogBufferCollection> createOplogBuffer(OperationContext* opCtx,
                                                         const UUID& migrationId) {
    OplogBufferCollection::Options options;
    options.peekCacheSize = static_cast<size_t>(tenantMigrationOplogBufferPeekCacheSize);
    options.dropCollectionAtStartup = false;
    options.dropCollectionAtShutdown = false;
    options.useTemporaryCollection = false;

    return std::make_shared<OplogBufferCollection>(
        StorageInterface::get(opCtx), getOplogBufferNs(migrationId), options);
}

}  // namespace

MONGO_FAIL_POINT_DEFINE(hangMigrationBeforeRetryCheck);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationRecipientInstanceBeforeDeletingOldStateDoc);

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

    StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx) const final {
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
    limits.minThreads = minTenantMigrationRecipientThreadPoolSize;
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

void TenantMigrationRecipientService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialStateDoc,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {
    auto tenantId = initialStateDoc["tenantId"].valueStringData();

    auto recipientStateDocument = TenantMigrationRecipientDocument::parse(
        IDLParserContext("recipientStateDoc"), initialStateDoc);

    for (auto& instance : existingInstances) {
        auto existingTypedInstance =
            checked_cast<const TenantMigrationRecipientService::Instance*>(instance);
        auto existingState = existingTypedInstance->getState();
        auto isDone = isMigrationCompleted(existingState.getState()) && existingState.getExpireAt();

        if (isDone) {
            continue;
        }

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "tenant " << tenantId << " is already migrating",
                existingTypedInstance->getTenantId() != tenantId);
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
      _stateDoc(
          TenantMigrationRecipientDocument::parse(IDLParserContext("recipientStateDoc"), stateDoc)),
      _tenantId(_stateDoc.getTenantId().toString()),
      _migrationUuid(_stateDoc.getId()),
      _donorConnectionString(_stateDoc.getDonorConnectionString().toString()),
      _donorUri(uassertStatusOK(MongoURI::parse(_stateDoc.getDonorConnectionString().toString()))),
      _readPreference(_stateDoc.getReadPreference()) {}

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
    bob.append("state", TenantMigrationRecipientState_serializer(_stateDoc.getState()));
    bob.append("migrationCompleted", _dataSyncCompletionPromise.getFuture().isReady());
    bob.append("garbageCollectable", _forgetMigrationDurablePromise.getFuture().isReady());
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
    auto stateDoc =
        TenantMigrationRecipientDocument::parse(IDLParserContext("recipientStateDoc"), options);

    invariant(stateDoc.getId() == _migrationUuid);

    if (stateDoc.getTenantId() != _tenantId ||
        stateDoc.getDonorConnectionString() != _donorConnectionString ||
        !stateDoc.getReadPreference().equals(_readPreference)) {
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
    auto swClientBase = ConnectionString(serverAddress).connect(applicationName);
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
    uassertStatusOK(replAuthenticate(clientBase)
                        .withContext(str::stream()
                                     << "TenantMigrationRecipientService failed to authenticate to "
                                     << serverAddress));
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

    if (!isMigrationCompleted(_stateDoc.getState())) {
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
    _stateDoc.setStartAt(_serviceContext->getFastClockSource()->now());

    return ExecutorFuture(**_scopedExecutor)
        .then([this, self = shared_from_this(), stateDoc = _stateDoc] {
            auto opCtx = cc().makeOperationContext();
            {
                Lock::ExclusiveLock stateDocInsertLock(opCtx.get(),
                                                       _recipientService->_stateDocInsertMutex);
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
                .waitUntilMajorityForWrite(
                    opCtx->getServiceContext(), writeOpTime, CancellationToken::uncancelable());
        })
        .semi();
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
    stdx::unique_lock lk(_mutex);
    // Get the last oplog entry at the read concern majority optime in the remote oplog. It
    // does not matter which tenant it is for.
    if (_sharedData->getResumePhase() != ResumePhase::kNone) {
        // We are resuming a migration.
        return SemiFuture<void>::makeReady();
    }

    lk.unlock();
    const auto startApplyingDonorOpTime = _getDonorMajorityOpTime(_client);
    LOGV2_DEBUG(4880600,
                2,
                "Found last oplog entry at read concern majority optime on remote node",
                "migrationId"_attr = getMigrationUUID(),
                "tenantId"_attr = _stateDoc.getTenantId(),
                "lastOplogEntry"_attr = startApplyingDonorOpTime.toBSON());

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
        startApplyingDonorOpTime = _stateDoc.getStartApplyingDonorOpTime()->getTimestamp();
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
    auto sessionTxnRecord = SessionTxnRecord::parse(IDLParserContext("SessionTxnRecord"), entry);
    auto sessionId = sessionTxnRecord.getSessionId();
    uassert(ErrorCodes::RetryableInternalTransactionNotSupported,
            str::stream()
                << "Tenant migration doesn't support retryable internal transaction. SessionId:: "
                << sessionId.toBSON(),
            !isInternalSessionForRetryableWrite(sessionId));

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

    const auto tenantNss = NamespaceStringUtil::deserialize(
        boost::none, getTenantId() + "_", SerializationContext::stateDefault());
    noopEntry.setNss(tenantNss);

    // Write a fake applyOps with the tenantId as the namespace so that this will be picked
    // up by the committed transaction prefetch pipeline in subsequent migrations.
    noopEntry.setObject(BSON(
        "applyOps" << BSON_ARRAY(BSON(OplogEntry::kNssFieldName << NamespaceStringUtil::serialize(
                                          tenantNss, SerializationContext::stateDefault())))));

    noopEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());
    noopEntry.setSessionId(sessionId);
    noopEntry.setTxnNumber(txnNumber);
    noopEntry.getOperationSessionInfo().setTxnRetryCounter(optTxnRetryCounter);

    // Use the same wallclock time as the noop entry.
    sessionTxnRecord.setStartOpTime(boost::none);
    sessionTxnRecord.setLastWriteDate(noopEntry.getWallClockTime());

    AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
    writeConflictRetry(
        opCtx, "writeDonorCommittedTxnEntry", NamespaceString::kRsOplogNamespace, [&] {
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

    auto cursor = _openCommittedTransactionsAggregationCursor();

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

std::unique_ptr<DBClientCursor>
TenantMigrationRecipientService::Instance::_openCommittedTransactionsAggregationCursor() {
    auto aggRequest = _makeCommittedTransactionsAggregation();

    auto statusWith = DBClientCursor::fromAggregationRequest(
        _client.get(), std::move(aggRequest), true /* secondaryOk */, false /* useExhaust */);
    if (!statusWith.isOK()) {
        LOGV2_ERROR(5351100,
                    "Fetch committed transactions aggregation failed",
                    "error"_attr = statusWith.getStatus());
        uassertStatusOKWithContext(statusWith.getStatus(),
                                   "Recipient migration instance committed transactions pre-fetch "
                                   "aggregation cursor failed");
    }

    return std::move(statusWith.getValue());
}

SemiFuture<void>
TenantMigrationRecipientService::Instance::_fetchRetryableWritesOplogBeforeStartOpTime() {
    _stopOrHangOnFailPoint(&fpAfterRetrievingStartOpTimesMigrationRecipientInstance);
    if (MONGO_unlikely(
            skipFetchingRetryableWritesEntriesBeforeStartOpTime.shouldFail())) {  // Test-only.
        return SemiFuture<void>::makeReady();
    }

    Timestamp startFetchingTimestamp;
    std::shared_ptr<OplogBufferCollection> donorOplogBuffer;
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

        invariant(_stateDoc.getStartFetchingDonorOpTime());
        startFetchingTimestamp = _stateDoc.getStartFetchingDonorOpTime().value().getTimestamp();
        donorOplogBuffer = _donorOplogBuffer;
    }

    auto opCtx = cc().makeOperationContext();
    auto expCtx = makeExpressionContext(opCtx.get());
    // If the oplog buffer contains entries at this point, it indicates that the recipient went
    // through failover before it finished writing all oplog entries to the buffer. Clear it and
    // redo the work.
    auto oplogBufferNS = getOplogBufferNs(getMigrationUUID());
    if (donorOplogBuffer->getCount() > 0) {
        // Ensure we are primary when trying to clear the oplog buffer since it will drop and
        // re-create the collection.
        auto coordinator = repl::ReplicationCoordinator::get(opCtx.get());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);
        if (!coordinator->canAcceptWritesForDatabase(opCtx.get(), oplogBufferNS.dbName())) {
            uassertStatusOK(
                Status(ErrorCodes::NotWritablePrimary,
                       "Recipient node is not primary, cannot clear oplog buffer collection."));
        }
        donorOplogBuffer->clear(opCtx.get());
    }

    LOGV2_DEBUG(5535300,
                1,
                "Pre-fetching retryable oplog entries before startFetchingTimstamp",
                "startFetchingTimestamp"_attr = startFetchingTimestamp,
                "tenantId"_attr = getTenantId(),
                "migrationId"_attr = getMigrationUUID());

    // Fetch the oplog chains of all retryable writes that occurred before startFetchingTimestamp.
    auto serializedPipeline = tenant_migration_util::createRetryableWritesOplogFetchingPipeline(
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

    std::unique_ptr<DBClientCursor> cursor = uassertStatusOKWithContext(
        DBClientCursor::fromAggregationRequest(
            _client.get(), std::move(aggRequest), true /* secondaryOk */, false /* useExhaust */),
        "Recipient migration instance retryable writes pre-fetch aggregation cursor failed");

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
            donorOplogBuffer->waitForSpace(opCtx.get(), toApplyDocumentBytes);
            // Buffer retryable writes entries.
            donorOplogBuffer->preload(
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
    _client.get()->runCommand(DatabaseName::kAdmin, cmd, readResult, QueryOption_SecondaryOk);
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
    std::shared_ptr<OplogBufferCollection> donorOplogBuffer;
    auto resumingFromOplogBuffer = false;
    {
        stdx::lock_guard lk(_mutex);
        _dataReplicatorExternalState =
            std::make_unique<DataReplicatorExternalStateTenantMigration>();
        startFetchOpTime = *_stateDoc.getStartFetchingDonorOpTime();
        donorOplogBuffer = _donorOplogBuffer;
    }

    if (_sharedData->getResumePhase() != ResumePhase::kNone) {
        // If the oplog buffer already contains fetched documents, we must be resuming a
        // migration.
        if (auto topOfOplogBuffer = donorOplogBuffer->lastObjectPushed(opCtx.get())) {
            startFetchOpTime =
                uassertStatusOK(OpTime::parseFromOplogEntry(topOfOplogBuffer.value()));
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
        const auto now = _serviceContext->getFastClockSource()->now();

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

    oplogFetcherConfig.queryFilter = _getOplogFetcherFilter();
    oplogFetcherConfig.requestResumeToken = true;

    oplogFetcherConfig.queryReadConcern =
        ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);
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
    uassertStatusOKWithContext(_donorOplogFetcher->startup(),
                               "Recipient migration instance oplog fetcher failed");

    lk.unlock();
    _stopOrHangOnFailPoint(&fpAfterStartingOplogFetcherMigrationRecipientInstance);
}

Status TenantMigrationRecipientService::Instance::_enqueueDocuments(
    OplogFetcher::Documents::const_iterator begin,
    OplogFetcher::Documents::const_iterator end,
    const OplogFetcher::DocumentsInfo& info) {

    auto donorOplogBuffer = [&]() {
        stdx::lock_guard lk(_mutex);
        invariant(_donorOplogBuffer);
        return _donorOplogBuffer;
    }();

    auto opCtx = cc().makeOperationContext();
    if (info.toApplyDocumentCount != 0) {
        // Buffer docs for later application.
        donorOplogBuffer->push(opCtx.get(), begin, end, info.toApplyDocumentBytes);
    }

    if (info.resumeToken.isNull()) {
        return Status(ErrorCodes::Error(5124600), "Resume token returned is null");
    }

    const auto lastPushedTS = donorOplogBuffer->getLastPushedTimestamp();
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
    donorOplogBuffer->push(opCtx.get(), noopVec.cbegin(), noopVec.cend(), boost::none);
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
            const auto now = _serviceContext->getFastClockSource()->now();
            _excludeDonorHost(lk,
                              _client->getServerHostAndPort(),
                              now + Milliseconds(tenantMigrationExcludeDonorHostTimeoutMS));
        }
        _interrupt(
            oplogFetcherStatus.withContext("Recipient migration instance oplog fetcher failed"),
            /*skipWaitingForForgetMigration=*/false);
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

TenantMigrationRecipientStateEnum
TenantMigrationRecipientService::Instance::_getTerminalStateFromFailpoint(FailPoint* fp) {
    TenantMigrationRecipientStateEnum state{TenantMigrationRecipientStateEnum::kUninitialized};

    fp->executeIf(
        [&](const BSONObj& data) {
            LOGV2(7221600,
                  "Tenant migration recipient instance: parsing state from failpoint",
                  "tenantId"_attr = getTenantId(),
                  "migrationId"_attr = getMigrationUUID(),
                  "name"_attr = fp->getName(),
                  "args"_attr = data);
            auto fpState = TenantMigrationRecipientState_parse(
                IDLParserContext("TenantMigrationRecipientStateEnum"), data["state"].str());
            switch (fpState) {
                case TenantMigrationRecipientStateEnum::kDone:
                case TenantMigrationRecipientStateEnum::kCommitted:
                case TenantMigrationRecipientStateEnum::kAborted:
                    state = fpState;
                    break;
                default:
                    LOGV2(7221601,
                          "Ignoring failpoint state because it is not a terminal state",
                          "fpState"_attr = fpState);
                    break;
            }
        },
        [&](const BSONObj& data) {
            auto state = data["state"].str();
            return !state.empty();
        });

    return state;
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

SemiFuture<void> TenantMigrationRecipientService::Instance::_onCloneSuccess() {
    _stopOrHangOnFailPoint(&fpBeforeMarkingCloneSuccess);
    stdx::lock_guard lk(_mutex);
    // PrimaryOnlyService::onStepUp() before starting instance makes sure that the state doc
    // is majority committed, so we can also skip waiting for it to be majority replicated.
    if (_sharedData->getResumePhase() == ResumePhase::kOplogCatchup) {
        return SemiFuture<void>::makeReady();
    }

    {
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
        _stateDoc.getDataConsistentStopDonorOpTime().value());
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

            auto donorConsistentOpTime = _stateDoc.getDataConsistentStopDonorOpTime();
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

template <class Promise, class Value>
void setPromiseValueIfNotReady(WithLock lk, Promise& promise, Value& value) {
    if (promise.getFuture().isReady()) {
        return;
    }

    promise.emplaceValue(value);
}

}  // namespace

SemiFuture<void> TenantMigrationRecipientService::Instance::_markStateDocAsGarbageCollectable() {
    _stopOrHangOnFailPoint(&fpAfterReceivingRecipientForgetMigration);

    // Throws if we have failed to persist the state doc at the first place. This can only happen in
    // unittests where we enable the autoRecipientForgetMigration failpoint. Otherwise,
    // recipientForgetMigration will always wait for the state doc to be persisted first and thus
    // this will only be called with _stateDocPersistedPromise resolved OK.
    invariant(_stateDocPersistedPromise.getFuture().isReady());
    _stateDocPersistedPromise.getFuture().get();

    invariant(_receivedRecipientForgetMigrationPromise.getFuture().isReady());
    auto nextState = _receivedRecipientForgetMigrationPromise.getFuture().get();

    stdx::lock_guard lk(_mutex);
    if (_stateDoc.getExpireAt()) {
        // Nothing to do if the state doc already has the expireAt set.
        return SemiFuture<void>::makeReady();
    }

    auto preImageDoc = _stateDoc.toBSON();

    return ExecutorFuture(**_scopedExecutor)
        .then([this, self = shared_from_this(), nextState, preImageDoc] {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();
            AutoGetCollection collection(
                opCtx, NamespaceString::kTenantMigrationRecipientsNamespace, MODE_IX);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << NamespaceString::kTenantMigrationRecipientsNamespace
                                         .toStringForErrorMsg()
                                  << " does not exist",
                    collection);

            writeConflictRetry(
                opCtx,
                "markTenantMigrationRecipientStateDocGarbageCollectable",
                NamespaceString::kTenantMigrationRecipientsNamespace,
                [&]() {
                    WriteUnitOfWork wuow(opCtx);
                    auto oplogSlot = LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];

                    auto stateDoc = [&]() {
                        stdx::lock_guard<Latch> lg(_mutex);
                        switch (nextState) {
                            case TenantMigrationRecipientStateEnum::kAborted:
                                _stateDoc.setAbortOpTime(oplogSlot);
                                break;
                            case TenantMigrationRecipientStateEnum::kCommitted:
                            case TenantMigrationRecipientStateEnum::kDone:
                                break;
                            default:
                                uasserted(ErrorCodes::InternalError,
                                          "Invalid state when marking state document "
                                          "as garbage collectable");
                                break;
                        }
                        _stateDoc.setState(nextState);
                        _stateDoc.setExpireAt(
                            _serviceContext->getFastClockSource()->now() +
                            Milliseconds{repl::tenantMigrationGarbageCollectionDelayMS.load()});

                        return _stateDoc.toBSON();
                    }();

                    const auto originalRecordId = Helpers::findOne(
                        opCtx, collection.getCollection(), BSON("_id" << _migrationUuid));
                    const auto originalSnapshot = Snapshotted<BSONObj>(
                        shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId(), preImageDoc);

                    invariant(!originalRecordId.isNull(),
                              str::stream() << "Existing tenant migration state document "
                                               "not found for id: "
                                            << getMigrationUUID());

                    CollectionUpdateArgs args{originalSnapshot.value()};
                    args.criteria = BSON("_id" << _migrationUuid);
                    args.oplogSlots = {oplogSlot};
                    args.update = stateDoc;

                    collection_internal::updateDocument(opCtx,
                                                        *collection,
                                                        originalRecordId,
                                                        originalSnapshot,
                                                        stateDoc,
                                                        collection_internal::kUpdateAllIndexes,
                                                        nullptr /* indexesAffected */,
                                                        nullptr /* OpDebug* */,
                                                        &args);


                    wuow.commit();
                });

            return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        })
        .then([this, self = shared_from_this()](repl::OpTime opTime) {
            return WaitForMajorityService::get(_serviceContext)
                .waitUntilMajorityForWrite(
                    _serviceContext, opTime, CancellationToken::uncancelable());
        })
        .onError([](Status status) {
            // We assume that we only fail with shutDown/stepDown errors (i.e. for
            // failovers). Otherwise, the whole chain would stop running without marking the
            // state doc garbage collectable while we are still the primary
            invariant(ErrorCodes::isShutdownError(status) || ErrorCodes::isNotPrimaryError(status));
            uassertStatusOK(status);
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

    // If the task is running, then setting promise result will be taken care by the main task
    // continuation chain.
    if (_taskState.isNotStarted()) {
        invariant(skipWaitingForForgetMigration);
        _stateDocPersistedPromise.setError(status);
        _dataConsistentPromise.setError(status);
        _dataSyncCompletionPromise.setError(status);

        // The interrupt() is called before the instance is scheduled to run. If the state doc has
        // already been marked garbage collectable, resolve the forget migration durable promise
        // with OK.
        if (_stateDoc.getExpireAt()) {
            _forgetMigrationDurablePromise.emplaceValue();
        } else {
            _forgetMigrationDurablePromise.setError(status);
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
    OperationContext* opCtx, const TenantMigrationRecipientStateEnum& nextState) {
    invariant(nextState == TenantMigrationRecipientStateEnum::kDone);
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
        _receivedRecipientForgetMigrationPromise.emplaceValue(nextState);
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
    {
        stdx::lock_guard lk(_mutex);
        _cancelRemainingWork(lk);

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
                .waitUntilMajorityForWrite(
                    opCtx->getServiceContext(), writeOpTime, CancellationToken::uncancelable());
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
        keyDocs.push_back(keys_collection_util::makeExternalClusterTimeKeyDoc(
            doc, _migrationUuid, boost::none /* expireAt */));
    }

    auto opCtx = cc().makeOperationContext();
    keys_collection_util::storeExternalClusterTimeKeyDocs(opCtx.get(), std::move(keyDocs));
}

SemiFuture<void>
TenantMigrationRecipientService::Instance::_checkIfFcvHasChangedSinceLastAttempt() {
    stdx::lock_guard lk(_mutex);

    // Record the FCV at the start of a migration and check for changes in every
    // subsequent attempt. Fail if there is any mismatch in FCV or
    // upgrade/downgrade state. (Generic FCV reference): This FCV check should
    // exist across LTS binary versions.
    auto currentFCV = serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
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
        MigrationProtocolEnum::kMultitenantMigrations,
        (!resumeOpTime.isNull()) ? std::max(resumeOpTime, *startApplyingDonorOpTime)
                                 : *startApplyingDonorOpTime,
        *cloneFinishedRecipientOpTime,
        _tenantId,
        _donorOplogBuffer.get(),
        **_scopedExecutor,
        _writerPool.get(),
        resumeOpTime.getTimestamp());

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
    std::shared_ptr<OplogBufferCollection> donorOplogBuffer;
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
            _serviceContext->getFastClockSource(), getMigrationUUID(), resumePhase);

        if (!_donorOplogBuffer) {
            _donorOplogBuffer = createOplogBuffer(opCtx, getMigrationUUID());
        }
        donorOplogBuffer = _donorOplogBuffer;
    }

    // Start the oplog buffer outside the mutex to avoid deadlock on a concurrent stepdown.
    try {
        // It is illegal to start the replicated donor buffer when the node is not primary.
        // So ensure we are primary before trying to startup the oplog buffer.
        repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);

        auto oplogBufferNS = getOplogBufferNs(getMigrationUUID());
        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(
                opCtx, oplogBufferNS.dbName())) {
            uassertStatusOK(
                Status(ErrorCodes::NotWritablePrimary, "Recipient node is no longer a primary."));
        }

        donorOplogBuffer->startup(opCtx);
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

SemiFuture<TenantOplogApplier::OpTimePair> TenantMigrationRecipientService::Instance::_migrate(
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

void TenantMigrationRecipientService::Instance::_dropTempCollections() {
    _stopOrHangOnFailPoint(&fpBeforeDroppingTempCollections);

    auto opCtx = cc().makeOperationContext();
    auto storageInterface = StorageInterface::get(opCtx.get());

    // The oplog buffer collection can be safely dropped at this point. In case either collection
    // does not exist, dropping will be a no-op. It isn't necessary that a given drop is
    // majority-committed. A new primary will attempt to drop the collection anyway.
    uassertStatusOK(
        storageInterface->dropCollection(opCtx.get(), getOplogBufferNs(getMigrationUUID())));
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
        if (serverGlobalParams.featureCompatibility.acquireFCVSnapshot()
                .isUpgradingOrDowngrading()) {
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
    if (isFCVUpgradingOrDowngrading()) {
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

                       TenantId tid = TenantId::parseFromString(_stateDoc.getTenantId());
                       auto mtab =
                           TenantMigrationAccessBlockerRegistry::get(_serviceContext)
                               .getTenantMigrationAccessBlockerForTenantId(
                                   tid, TenantMigrationAccessBlocker::BlockerType::kRecipient);
                       if (mtab && mtab->getMigrationId() != _migrationUuid) {
                           // There is a conflicting migration. If its state doc has already
                           // been marked as garbage collectable, this instance must correspond
                           // to a retry and we can delete immediately to allow the migration to
                           // restart. Otherwise, there is a real conflict so we should throw
                           // ConflictingInProgress.
                           lk.unlock();

                           auto existingStateDoc =
                               tenantMigrationRecipientEntryHelpers::getStateDoc(
                                   opCtx.get(), mtab->getMigrationId());
                           uassertStatusOK(existingStateDoc.getStatus());

                           uassert(ErrorCodes::ConflictingOperationInProgress,
                                   str::stream()
                                       << "Found active migration for tenantId \"" << _tenantId
                                       << "\" with migration id " << mtab->getMigrationId(),
                                   existingStateDoc.getValue().getExpireAt());

                           pauseTenantMigrationRecipientInstanceBeforeDeletingOldStateDoc
                               .pauseWhileSet();

                           auto deleted =
                               uassertStatusOK(tenantMigrationRecipientEntryHelpers::
                                                   deleteStateDocIfMarkedAsGarbageCollectable(
                                                       opCtx.get(), _tenantId));
                           // The doc has an expireAt but was deleted before we had time to
                           // delete it above therefore it's safe to pursue since it has been
                           // cleaned up.
                           if (!deleted) {
                               LOGV2_WARNING(6792601,
                                             "Existing state document was deleted before we could "
                                             "delete it ourselves.");
                           }
                           lk.lock();
                       }

                       if (_stateDoc.getState() !=
                               TenantMigrationRecipientStateEnum::kUninitialized &&
                           !isMigrationCompleted(_stateDoc.getState()) &&
                           !_stateDocPersistedPromise.getFuture().isReady() &&
                           !_stateDoc.getExpireAt()) {
                           // If our state is initialized and we haven't fulfilled the
                           // '_stateDocPersistedPromise' yet, it means we are restarting the future
                           // chain due to recipient failover.
                           _stateDocPersistedPromise.emplaceValue();
                           uasserted(ErrorCodes::TenantMigrationAborted,
                                     str::stream() << "Recipient failover happened during "
                                                      "migration :: migrationId: "
                                                   << getMigrationUUID());
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
                                   !isMigrationCompleted(_stateDoc.getState()) &&
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
                   .then([this, self = shared_from_this(), token] { return _migrate(token); });
           })
        .until([this, self = shared_from_this()](
                   StatusOrStatusWith<TenantOplogApplier::OpTimePair> applierStatus) {
            hangMigrationBeforeRetryCheck.pauseWhileSet();

            auto shouldRetryMigration = [&](WithLock, Status status) -> bool {
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

            // Handle recipientForgetMigration.
            stdx::lock_guard lk(_mutex);
            if (_stateDoc.getExpireAt()) {
                auto state = _stateDoc.getState();
                setPromiseValueIfNotReady(lk, _receivedRecipientForgetMigrationPromise, state);
            } else if (status.code() == ErrorCodes::ConflictingServerlessOperation) {
                auto state = TenantMigrationRecipientStateEnum::kDone;
                setPromiseValueIfNotReady(lk, _receivedRecipientForgetMigrationPromise, state);
            } else if (MONGO_unlikely(autoRecipientForgetMigration.shouldFail())) {
                auto state = _getTerminalStateFromFailpoint(&autoRecipientForgetMigration);
                if (state != TenantMigrationRecipientStateEnum::kUninitialized) {
                    setPromiseValueIfNotReady(lk, _receivedRecipientForgetMigrationPromise, state);
                }
            }
        })
        .thenRunOn(**_scopedExecutor)
        .then([this, self = shared_from_this()] {
            // Schedule on the _scopedExecutor to make sure we are still the primary when
            // waiting for the recipientForgetMigration command.
            return _receivedRecipientForgetMigrationPromise.getFuture();
        })
        .then([this, self = shared_from_this(), token](
                  const boost::optional<TenantMigrationRecipientStateEnum>& state) {
            auto expireAt = [&]() {
                stdx::lock_guard<Latch> lg(_mutex);
                return _stateDoc.getExpireAt();
            }();
            if (expireAt) {
                return ExecutorFuture(**_scopedExecutor);
            }
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
        .then([this, self = shared_from_this()] { _dropTempCollections(); })
        .then([this, self = shared_from_this(), token] {
            {
                stdx::lock_guard lk(_mutex);
                setPromiseOkifNotReady(lk, _forgetMigrationDurablePromise);
            }
            return _waitForGarbageCollectionDelayThenDeleteStateDoc(token);
        })
        .thenRunOn(_recipientService->getInstanceCleanupExecutor())
        .onCompletion([this,
                       self = shared_from_this(),
                       scopedCounter{std::move(scopedOutstandingMigrationCounter)}](Status status) {
            // Schedule on the parent executor to mark the completion of the whole chain so this
            // is safe even on shutDown/stepDown.
            stdx::lock_guard lk(_mutex);
            invariant(_dataSyncCompletionPromise.getFuture().isReady());

            if (status == ErrorCodes::ConflictingServerlessOperation) {
                LOGV2(6531506,
                      "Migration failed as another serverless operation was in progress",
                      "migrationId"_attr = getMigrationUUID(),
                      "tenantId"_attr = getTenantId(),
                      "status"_attr = status);
                setPromiseOkifNotReady(lk, _forgetMigrationDurablePromise);
                return status;
            }

            if (status == ErrorCodes::CallbackCanceled) {
                // Replace the CallbackCanceled error with InterruptedDueToReplStateChange to
                // support retry behavior. We can receive a CallbackCanceled error here during
                // a failover if the ScopedTaskExecutor is shut down and rejects a task before
                // the OperationContext is interrupted.
                status = Status{ErrorCodes::InterruptedDueToReplStateChange,
                                "operation was interrupted"};
            }

            if (!status.isOK()) {
                LOGV2(4881402,
                      "Migration not marked to be garbage collectable",
                      "migrationId"_attr = getMigrationUUID(),
                      "tenantId"_attr = getTenantId(),
                      "status"_attr = status);
                setPromiseErrorifNotReady(lk, _forgetMigrationDurablePromise, status);
            }

            _taskState.setState(TaskState::kDone);

            return Status::OK();
        })
        .semi();
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_removeStateDoc(
    const CancellationToken& token) {
    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               pauseTenantMigrationRecipientBeforeDeletingStateDoc.pauseWhileSet(opCtx);

               PersistentTaskStore<TenantMigrationRecipientDocument> store(_stateDocumentsNS);
               store.remove(
                   opCtx,
                   BSON(TenantMigrationRecipientDocument::kIdFieldName << _migrationUuid),
                   WriteConcernOptions(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)));
               LOGV2(8423371,
                     "State document is deleted",
                     "migrationId"_attr = _migrationUuid,
                     "tenantId"_attr = _tenantId);
           })
        .until([](Status status) { return status.isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**_scopedExecutor, token)
        .semi();
}

SemiFuture<void>
TenantMigrationRecipientService::Instance::_waitForGarbageCollectionDelayThenDeleteStateDoc(
    const CancellationToken& token) {
    stdx::lock_guard<Latch> lg(_mutex);
    LOGV2(8423369,
          "Waiting for garbage collection delay before deleting state document",
          "migrationId"_attr = _migrationUuid,
          "tenantId"_attr = _tenantId,
          "expireAt"_attr = *_stateDoc.getExpireAt());

    return (**_scopedExecutor)
        ->sleepUntil(*_stateDoc.getExpireAt(), token)
        .then([this, self = shared_from_this(), token]() {
            LOGV2(8423370,
                  "Deleting state document",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId);
            return _removeStateDoc(token);
        })
        .semi();
}

const UUID& TenantMigrationRecipientService::Instance::getMigrationUUID() const {
    return _migrationUuid;
}

const std::string& TenantMigrationRecipientService::Instance::getTenantId() const {
    return _tenantId;
}

TenantMigrationRecipientDocument TenantMigrationRecipientService::Instance::getState() const {
    stdx::lock_guard lk(_mutex);
    return _stateDoc;
}

}  // namespace repl
}  // namespace mongo
