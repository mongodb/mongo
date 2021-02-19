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

#include "mongo/platform/basic.h"

#include "mongo/base/checked_cast.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/config.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
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
#include "mongo/db/repl/tenant_migration_recipient_entry_helpers.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_util.h"

namespace mongo {
namespace repl {
namespace {
const std::string kTTLIndexName = "TenantMigrationRecipientTTLIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());
constexpr StringData kOplogBufferPrefix = "repl.migration.oplog_"_sd;

NamespaceString getOplogBufferNs(const UUID& migrationUUID) {
    return NamespaceString(NamespaceString::kConfigDb,
                           kOplogBufferPrefix + migrationUUID.toString());
}
}  // namespace

// A convenient place to set test-specific parameters.
MONGO_FAIL_POINT_DEFINE(pauseBeforeRunTenantMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(pauseAfterRunTenantMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(skipTenantMigrationRecipientAuth);
MONGO_FAIL_POINT_DEFINE(skipComparingRecipientAndDonorFCV);
MONGO_FAIL_POINT_DEFINE(autoRecipientForgetMigration);
MONGO_FAIL_POINT_DEFINE(pauseAfterCreatingOplogBuffer);

// Fails before waiting for the state doc to be majority replicated.
MONGO_FAIL_POINT_DEFINE(failWhilePersistingTenantMigrationRecipientInstanceStateDoc);
MONGO_FAIL_POINT_DEFINE(fpAfterPersistingTenantMigrationRecipientInstanceStateDoc);
MONGO_FAIL_POINT_DEFINE(fpAfterConnectingTenantMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(fpAfterRecordingRecipientPrimaryStartingFCV);
MONGO_FAIL_POINT_DEFINE(fpAfterComparingRecipientAndDonorFCV);
MONGO_FAIL_POINT_DEFINE(fpAfterRetrievingStartOpTimesMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(fpAfterStartingOplogFetcherMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(setTenantMigrationRecipientInstanceHostTimeout);
MONGO_FAIL_POINT_DEFINE(pauseAfterRetrievingLastTxnMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(fpAfterCollectionClonerDone);
MONGO_FAIL_POINT_DEFINE(fpAfterStartingOplogApplierMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(fpBeforeFulfillingDataConsistentPromise);
MONGO_FAIL_POINT_DEFINE(fpAfterDataConsistentMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(hangBeforeTaskCompletion);
MONGO_FAIL_POINT_DEFINE(fpAfterReceivingRecipientForgetMigration);
MONGO_FAIL_POINT_DEFINE(hangAfterCreatingRSM);
MONGO_FAIL_POINT_DEFINE(skipRetriesWhenConnectingToDonorHost);
MONGO_FAIL_POINT_DEFINE(fpBeforeDroppingOplogBufferCollection);
MONGO_FAIL_POINT_DEFINE(fpWaitUntilTimestampMajorityCommitted);

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
                         rpc::OplogQueryMetadata oqMetadata) final {}

    // Tenant migration does not change sync source depending on metadata.
    ChangeSyncSourceAction shouldStopFetching(const HostAndPort& source,
                                              const rpc::ReplSetMetadata& replMetadata,
                                              const rpc::OplogQueryMetadata& oqMetadata,
                                              const OpTime& previousOpTimeFetched,
                                              const OpTime& lastOpTimeFetched) final {
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

ExecutorFuture<void> TenantMigrationRecipientService::_rebuildService(
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
        .until([token](Status status) { return status.isOK() || token.isCanceled(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

std::shared_ptr<PrimaryOnlyService::Instance> TenantMigrationRecipientService::constructInstance(
    BSONObj initialStateDoc) const {
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
      _migrationUuid(_stateDoc.getId()),
      _donorConnectionString(_stateDoc.getDonorConnectionString().toString()),
      _donorUri(uassertStatusOK(MongoURI::parse(_stateDoc.getDonorConnectionString().toString()))),
      _readPreference(_stateDoc.getReadPreference()),
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
    bob.append("instanceID", _stateDoc.getId().toBSON());
    bob.append("tenantId", _stateDoc.getTenantId());
    bob.append("donorConnectionString", _stateDoc.getDonorConnectionString());
    bob.append("readPreference", _stateDoc.getReadPreference().toInnerBSON());
    bob.append("state", _stateDoc.getState());
    bob.append("dataSyncCompleted", _dataSyncCompletionPromise.getFuture().isReady());
    bob.append("migrationCompleted", _taskCompletionPromise.getFuture().isReady());

    if (_stateDoc.getStartFetchingDonorOpTime())
        bob.append("startFetchingDonorOpTime", _stateDoc.getStartFetchingDonorOpTime()->toBSON());
    if (_stateDoc.getStartApplyingDonorOpTime())
        bob.append("startApplyingDonorOpTime", _stateDoc.getStartApplyingDonorOpTime()->toBSON());
    if (_stateDoc.getDataConsistentStopDonorOpTime())
        bob.append("dataConsistentStopDonorOpTime",
                   _stateDoc.getDataConsistentStopDonorOpTime()->toBSON());
    if (_stateDoc.getCloneFinishedRecipientOpTime())
        bob.append("cloneFinishedRecipientOpTime",
                   _stateDoc.getCloneFinishedRecipientOpTime()->toBSON());

    if (_stateDoc.getExpireAt())
        bob.append("expireAt", _stateDoc.getExpireAt()->toString());

    if (_client) {
        bob.append("donorSyncSource", _client->getServerAddress());
    }

    return bob.obj();
}

Status TenantMigrationRecipientService::Instance::checkIfOptionsConflict(
    const TenantMigrationRecipientDocument& requestedStateDoc) const {
    invariant(requestedStateDoc.getId() == _migrationUuid);

    if (requestedStateDoc.getTenantId() == _tenantId &&
        requestedStateDoc.getDonorConnectionString() == _donorConnectionString &&
        requestedStateDoc.getReadPreference().equals(_readPreference)) {
        return Status::OK();
    }

    return Status(ErrorCodes::ConflictingOperationInProgress,
                  str::stream() << "Requested options for tenant migration doesn't match"
                                << " the active migration options, migrationId: " << _migrationUuid
                                << ", tenantId: " << _tenantId
                                << ", connectionString: " << _donorConnectionString
                                << ", readPreference: " << _readPreference.toString()
                                << ", requested options:" << requestedStateDoc.toBSON());
}

OpTime TenantMigrationRecipientService::Instance::waitUntilMigrationReachesConsistentState(
    OperationContext* opCtx) const {
    return _dataConsistentPromise.getFuture().get(opCtx);
}

OpTime TenantMigrationRecipientService::Instance::waitUntilTimestampIsMajorityCommitted(
    OperationContext* opCtx, const Timestamp& donorTs) const {

    // This gives assurance that _tenantOplogApplier pointer won't be empty, and that it has been
    // started. Additionally, we must have finished processing the recipientSyncData command that
    // waits on _dataConsistentPromise.
    _dataConsistentPromise.getFuture().get(opCtx);

    auto getWaitOpTimeFuture = [&]() {
        stdx::lock_guard lk(_mutex);

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
            OpTime(donorTs, OpTime::kUninitializedTerm));
    };

    auto waitOpTimeFuture = getWaitOpTimeFuture();
    fpWaitUntilTimestampMajorityCommitted.pauseWhileSet();
    auto swDonorRecipientOpTimePair = waitOpTimeFuture.getNoThrow();

    auto status = swDonorRecipientOpTimePair.getStatus();

    // A cancelation error may occur due to an interrupt. If that is the case, replace the error
    // code with the interrupt code, the true reason for interruption.
    if (ErrorCodes::isCancelationError(status)) {
        stdx::lock_guard lk(_mutex);
        if (!_taskState.getInterruptStatus().isOK()) {
            status = _taskState.getInterruptStatus();
        }
    }

    uassertStatusOK(status);

    // We want to guarantee that the recipient logical clock has advanced to at least the donor
    // timestamp before returning success for recipientSyncData by doing a majority committed noop
    // write after ticking the recipient clock to the donor timestamp.
    // Note: tickClusterTimeTo() will not tick the recipient clock backwards in time.
    VectorClockMutable::get(opCtx)->tickClusterTimeTo(LogicalTime(donorTs));

    BSONObj result;
    DBDirectClient client(opCtx);
    client.runCommand(NamespaceString::kAdminDb.toString(),
                      BSON("appendOplogNote" << 1 << "data"
                                             << BSON("msg"
                                                     << "Noop write for recipientSyncData")),
                      result);
    uassertStatusOK(getStatusFromCommandResult(result));

    // Wait for the noop write optime to be majority committed.
    WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajority(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp())
        .get(opCtx);
    return swDonorRecipientOpTimePair.getValue().donorOpTime;
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
    auto majorityOpTimeBson =
        client->findOne(NamespaceString::kRsOplogNamespace.ns(),
                        Query().sort("$natural", -1),
                        &oplogOpTimeFields,
                        QueryOption_SecondaryOk,
                        ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    uassert(5272003, "Found no entries in the remote oplog", !majorityOpTimeBson.isEmpty());

    auto majorityOpTime = uassertStatusOK(OpTime::parseFromOplogEntry(majorityOpTimeBson));
    return majorityOpTime;
}

SemiFuture<TenantMigrationRecipientService::Instance::ConnectionPair>
TenantMigrationRecipientService::Instance::_createAndConnectClients() {
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
    CancelationSource getHostCancelSource;
    setTenantMigrationRecipientInstanceHostTimeout.execute([&](const BSONObj& data) {
        auto exec = **_scopedExecutor;
        const auto deadline =
            exec->now() + Milliseconds(data["findHostTimeoutMillis"].safeNumberLong());
        // Cancel the find host request after a timeout. Ignore callback handle.
        exec->sleepUntil(deadline, CancelationToken::uncancelable())
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
                   });
           })
        .until([this, self = shared_from_this(), kDelayedMajorityOpTimeErrorCode](
                   const StatusWith<ConnectionPair>& statusWith) {
            auto status = statusWith.getStatus();

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
        .on(**_scopedExecutor, CancelationToken::uncancelable())
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
    // If the instance state is not 'kUninitialized', then the instance is restarted by step
    // up. So, skip persisting the state doc. And, PrimaryOnlyService::onStepUp() waits for
    // majority commit of the primary no-op oplog entry written by the node in the newer
    // term before scheduling the Instance::run(). So, it's also safe to assume that
    // instance's state document written in an older term on disk won't get rolled back for
    // step up case.
    if (_stateDoc.getState() != TenantMigrationRecipientStateEnum::kUninitialized) {
        return SemiFuture<void>::makeReady();
    }

    LOGV2_DEBUG(5081400,
                2,
                "Recipient migration service initializing state document",
                "tenantId"_attr = getTenantId(),
                "migrationId"_attr = getMigrationUUID(),
                "connectionString"_attr = _donorConnectionString,
                "readPreference"_attr = _readPreference);

    // Persist the state doc before starting the data sync.
    _stateDoc.setState(TenantMigrationRecipientStateEnum::kStarted);


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
                .waitUntilMajority(writeOpTime);
        })
        .semi();
}

void TenantMigrationRecipientService::Instance::_getStartOpTimesFromDonor(WithLock lk) {
    if (_sharedData->isResuming()) {
        // We are resuming a migration.
        return;
    }
    // We only expect to already have start optimes populated if we are resuming a migration.
    invariant(!_stateDoc.getStartApplyingDonorOpTime().has_value());
    invariant(!_stateDoc.getStartFetchingDonorOpTime().has_value());
    // Get the last oplog entry at the read concern majority optime in the remote oplog.  It
    // does not matter which tenant it is for.
    auto lastOplogEntry1OpTime = _getDonorMajorityOpTime(_client);
    LOGV2_DEBUG(4880600,
                2,
                "Found last oplog entry at read concern majority optime on remote node",
                "migrationId"_attr = getMigrationUUID(),
                "tenantId"_attr = _stateDoc.getTenantId(),
                "lastOplogEntry"_attr = lastOplogEntry1OpTime.toBSON());

    // Get the optime of the earliest transaction that was open at the read concern majority optime
    // As with the last oplog entry, it does not matter that this may be for a different tenant; an
    // optime that is too early does not result in incorrect behavior.
    const auto preparedState = DurableTxnState_serializer(DurableTxnStateEnum::kPrepared);
    const auto inProgressState = DurableTxnState_serializer(DurableTxnStateEnum::kInProgress);
    auto transactionTableOpTimeFields = BSON(SessionTxnRecord::kStartOpTimeFieldName << 1);
    auto earliestOpenTransactionBson = _client->findOne(
        NamespaceString::kSessionTransactionsTableNamespace.ns(),
        QUERY("state" << BSON("$in" << BSON_ARRAY(preparedState << inProgressState)))
            .sort(SessionTxnRecord::kStartOpTimeFieldName.toString(), 1),
        &transactionTableOpTimeFields,
        QueryOption_SecondaryOk,
        ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    LOGV2_DEBUG(4880602,
                2,
                "Transaction table entry for earliest transaction that was open at the read "
                "concern majority optime on remote node (may be empty)",
                "migrationId"_attr = getMigrationUUID(),
                "tenantId"_attr = _stateDoc.getTenantId(),
                "earliestOpenTransaction"_attr = earliestOpenTransactionBson);

    pauseAfterRetrievingLastTxnMigrationRecipientInstance.pauseWhileSet();

    // We need to fetch the last oplog entry both before and after getting the transaction
    // table entry, as otherwise there is a potential race where we may try to apply
    // a commit for which we have not fetched a previous transaction oplog entry.
    auto lastOplogEntry2OpTime = _getDonorMajorityOpTime(_client);
    LOGV2_DEBUG(4880604,
                2,
                "Found last oplog entry at the read concern majority optime (after reading txn "
                "table) on remote node",
                "migrationId"_attr = getMigrationUUID(),
                "tenantId"_attr = _stateDoc.getTenantId(),
                "lastOplogEntry"_attr = lastOplogEntry2OpTime.toBSON());
    _stateDoc.setStartApplyingDonorOpTime(lastOplogEntry2OpTime);

    OpTime startFetchingDonorOpTime = lastOplogEntry1OpTime;
    if (!earliestOpenTransactionBson.isEmpty()) {
        auto startOpTimeField =
            earliestOpenTransactionBson[SessionTxnRecord::kStartOpTimeFieldName];
        if (startOpTimeField.isABSONObj()) {
            startFetchingDonorOpTime = OpTime::parse(startOpTimeField.Obj());
        }
    }
    _stateDoc.setStartFetchingDonorOpTime(startFetchingDonorOpTime);
}

void TenantMigrationRecipientService::Instance::_fetchRetryableWritesOplogBeforeStartOpTime() {
    // TODO SERVER-53319: Run the aggregation pipeline on the correct tenant donor's oplog. Move
    // oplog buffer creation here and add the returned oplog entries to the buffer.
    return;
}

void TenantMigrationRecipientService::Instance::_fetchCommittedTransactionsBeforeStartOpTime() {
    // TODO (SERVER-53511): Run the aggregation.
    return;
}

void TenantMigrationRecipientService::Instance::_startOplogFetcher() {
    auto opCtx = cc().makeOperationContext();
    OplogBufferCollection::Options options;
    options.peekCacheSize = static_cast<size_t>(tenantMigrationOplogBufferPeekCacheSize);
    options.dropCollectionAtStartup = false;
    options.dropCollectionAtShutdown = false;
    options.useTemporaryCollection = false;
    // Create the oplog buffer outside the mutex to avoid deadlock on a concurrent stepdown.
    auto oplogBufferNS = getOplogBufferNs(getMigrationUUID());
    auto bufferCollection = std::make_unique<OplogBufferCollection>(
        StorageInterface::get(opCtx.get()), oplogBufferNS, options);
    stdx::unique_lock lk(_mutex);
    invariant(_stateDoc.getStartFetchingDonorOpTime());
    _donorOplogBuffer = std::move(bufferCollection);

    {
        // Ensure we are primary when trying to startup and create the oplog buffer collection.
        auto coordinator = repl::ReplicationCoordinator::get(opCtx.get());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);
        if (!coordinator->canAcceptWritesForDatabase(opCtx.get(), oplogBufferNS.db())) {
            uassertStatusOK(
                Status(ErrorCodes::NotWritablePrimary,
                       "Recipient node is not primary, cannot create oplog buffer collection."));
        }
        _donorOplogBuffer->startup(opCtx.get());
    }

    pauseAfterCreatingOplogBuffer.pauseWhileSet();

    _dataReplicatorExternalState = std::make_unique<DataReplicatorExternalStateTenantMigration>();
    auto startFetchOpTime = *_stateDoc.getStartFetchingDonorOpTime();
    auto resumingFromOplogBuffer = false;
    if (_sharedData->isResuming()) {
        // Release the mutex lock since we acquire a collection mode IS lock when checking the last
        // object pushed in the oplog buffer.
        lk.unlock();
        // If the oplog buffer already contains fetched documents, we must be resuming a migration.
        if (auto topOfOplogBuffer = _donorOplogBuffer->lastObjectPushed(opCtx.get())) {
            startFetchOpTime = uassertStatusOK(OpTime::parseFromOplogEntry(topOfOplogBuffer.get()));
            resumingFromOplogBuffer = true;
        }
        lk.lock();
    }
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
        OplogFetcher::RequireFresherSyncSource::kDontRequireFresherSyncSource);
    oplogFetcherConfig.queryFilter = _getOplogFetcherFilter();
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
        _interrupt(oplogFetcherStatus, /*skipWaitingForForgetMigration=*/false);
    }
}

void TenantMigrationRecipientService::Instance::_stopOrHangOnFailPoint(FailPoint* fp) {
    fp->executeIf(
        [&](const BSONObj& data) {
            LOGV2(4881103,
                  "Tenant migration recipient instance: failpoint enabled",
                  "tenantId"_attr = getTenantId(),
                  "migrationId"_attr = getMigrationUUID(),
                  "name"_attr = fp->getName(),
                  "args"_attr = data);
            if (data["action"].str() == "hang") {
                fp->pauseWhileSet();
            } else {
                uasserted(data["stopErrorCode"].numberInt(),
                          "Skipping remaining processing due to fail point");
            }
        },
        [&](const BSONObj& data) {
            auto action = data["action"].str();
            return (action == "hang" || action == "stop");
        });
}

bool TenantMigrationRecipientService::Instance::_isCloneCompletedMarkerSet(WithLock) const {
    return _stateDoc.getCloneFinishedRecipientOpTime().has_value();
}

OpTime TenantMigrationRecipientService::Instance::_getOplogResumeApplyingDonorOptime(
    const OpTime startApplyingDonorOpTime, const OpTime cloneFinishedRecipientOpTime) const {
    invariant(_stateDoc.getCloneFinishedRecipientOpTime().has_value());
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
            (oplogObj.getStringField("op") == OpType_serializer(repl::OpTypeEnum::kNoop))) {
            const auto migratedEntryObj = oplogObj.getObjectField("o");
            const auto swDonorOpTime = repl::OpTime::parseFromOplogEntry(migratedEntryObj);
            uassert(5272305,
                    str::stream() << "Unable to parse opTime from tenant migration oplog entry: "
                                  << redact(oplogObj) << ", error: " << swDonorOpTime.getStatus(),
                    swDonorOpTime.isOK());
            if (swDonorOpTime.getValue() < startApplyingDonorOpTime) {
                break;
            }
            LOGV2_DEBUG(5272302,
                        1,
                        "Found an optime to resume oplog application from",
                        "opTime"_attr = swDonorOpTime.getValue());
            return swDonorOpTime.getValue();
        }
        result = oplogIter->next();
    }
    LOGV2_DEBUG(5272304,
                1,
                "Resuming oplog application from startApplyingDonorOpTime",
                "opTime"_attr = startApplyingDonorOpTime);
    return startApplyingDonorOpTime;
}

Future<void> TenantMigrationRecipientService::Instance::_startTenantAllDatabaseCloner(WithLock lk) {
    // If the state is data consistent, do not start the cloner.
    if (_isCloneCompletedMarkerSet(lk)) {
        return {Future<void>::makeReady()};
    }

    auto opCtx = cc().makeOperationContext();
    _tenantAllDatabaseCloner =
        std::make_unique<TenantAllDatabaseCloner>(_sharedData.get(),
                                                  _client->getServerHostAndPort(),
                                                  _client.get(),
                                                  repl::StorageInterface::get(opCtx.get()),
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
    stdx::lock_guard lk(_mutex);
    // PrimaryOnlyService::onStepUp() before starting instance makes sure that the state doc
    // is majority committed, so we can also skip waiting for it to be majority replicated.
    if (_isCloneCompletedMarkerSet(lk)) {
        return SemiFuture<void>::makeReady();
    }

    auto opCtx = cc().makeOperationContext();
    {
        stdx::lock_guard<TenantMigrationSharedData> sharedDatalk(*_sharedData);
        auto lastVisibleMajorityCommittedDonorOpTime =
            _sharedData->getLastVisibleOpTime(sharedDatalk);
        invariant(!lastVisibleMajorityCommittedDonorOpTime.isNull());
        _stateDoc.setDataConsistentStopDonorOpTime(lastVisibleMajorityCommittedDonorOpTime);
    }
    _stateDoc.setCloneFinishedRecipientOpTime(
        repl::ReplicationCoordinator::get(opCtx.get())->getMyLastAppliedOpTime());

    return ExecutorFuture(**_scopedExecutor)
        .then([this, self = shared_from_this(), stateDoc = _stateDoc] {
            auto opCtx = cc().makeOperationContext();
            uassertStatusOK(
                tenantMigrationRecipientEntryHelpers::updateStateDoc(opCtx.get(), stateDoc));

            auto writeOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            return WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(writeOpTime);
        })
        .semi();
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_getDataConsistentFuture() {
    stdx::lock_guard lk(_mutex);
    // PrimaryOnlyService::onStepUp() before starting instance makes sure that the state doc
    // is majority committed, so we can also skip waiting for it to be majority replicated.
    if (_stateDoc.getState() == TenantMigrationRecipientStateEnum::kConsistent) {
        return SemiFuture<void>::makeReady();
    }

    return _tenantOplogApplier
        ->getNotificationForOpTime(_stateDoc.getDataConsistentStopDonorOpTime().get())
        .thenRunOn(**_scopedExecutor)
        .then(
            [this, self = shared_from_this()](TenantOplogApplier::OpTimePair donorRecipientOpTime) {
                stdx::lock_guard lk(_mutex);
                // Persist the state that tenant migration instance has reached
                // consistent state.
                _stateDoc.setState(TenantMigrationRecipientStateEnum::kConsistent);
                return _stateDoc;
            })
        .then([this, self = shared_from_this()](TenantMigrationRecipientDocument stateDoc) {
            auto opCtx = cc().makeOperationContext();
            uassertStatusOK(
                tenantMigrationRecipientEntryHelpers::updateStateDoc(opCtx.get(), stateDoc));
            return WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());
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
                .waitUntilMajority(writeOpTime);
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
        // We only get here on receiving the recipientForgetMigration command or on
        // stepDown/shutDown. On receiving the recipientForgetMigration, the promise should have
        // already been set.
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
        _dataSyncStartedPromise.setError(status);
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
    {
        stdx::lock_guard lk(_mutex);

        _cancelRemainingWork(lk);

        shutdownTarget(lk, _donorOplogFetcher);
        shutdownTargetWithOpCtx(lk, _donorOplogBuffer, opCtx.get());

        _donorReplicaSetMonitor = nullptr;

        invariant(!status.isOK());
        setPromiseErrorifNotReady(lk, _stateDocPersistedPromise, status);
        setPromiseErrorifNotReady(lk, _dataSyncStartedPromise, status);
        setPromiseErrorifNotReady(lk, _dataConsistentPromise, status);
        setPromiseErrorifNotReady(lk, _dataSyncCompletionPromise, status);

        shutdownTarget(lk, _writerPool);

        // Save them to join() with it outside of _mutex.
        using std::swap;
        swap(savedDonorOplogFetcher, _donorOplogFetcher);
        swap(savedTenantOplogApplier, _tenantOplogApplier);
        swap(savedWriterPool, _writerPool);
    }

    // Perform join outside the lock to avoid deadlocks.
    joinTarget(savedDonorOplogFetcher);
    joinTarget(savedTenantOplogApplier);
    joinTarget(savedWriterPool);
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
                .waitUntilMajority(writeOpTime);
        })
        .semi();
}

void TenantMigrationRecipientService::Instance::_fetchAndStoreDonorClusterTimeKeyDocs(
    const CancelationToken& token) {
    std::vector<ExternalKeysCollectionDocument> keyDocs;

    auto cursor =
        _client->query(NamespaceString::kKeysCollectionNamespace,
                       Query().readPref(_readPreference.pref, _readPreference.tags.getTagBSON()));
    while (cursor->more()) {
        const auto doc = cursor->nextSafe().getOwned();
        keyDocs.push_back(tenant_migration_util::makeExternalClusterTimeKeyDoc(
            _serviceContext, _migrationUuid, doc));
    }

    tenant_migration_util::storeExternalClusterTimeKeyDocs(_scopedExecutor, std::move(keyDocs));
}

void TenantMigrationRecipientService::Instance::_compareRecipientAndDonorFCV() const {
    if (skipComparingRecipientAndDonorFCV.shouldFail()) {  // Test-only.
        return;
    }

    auto donorFCVbson =
        _client->findOne(NamespaceString::kServerConfigurationNamespace.ns(),
                         QUERY("_id" << FeatureCompatibilityVersionParser::kParameterName),
                         nullptr,
                         QueryOption_SecondaryOk,
                         ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());

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

SemiFuture<void> TenantMigrationRecipientService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancelationToken& token) noexcept {
    _scopedExecutor = executor;

    LOGV2(4879607,
          "Starting tenant migration recipient instance: ",
          "migrationId"_attr = getMigrationUUID(),
          "tenantId"_attr = getTenantId(),
          "connectionString"_attr = _donorConnectionString,
          "readPreference"_attr = _readPreference);

    pauseBeforeRunTenantMigrationRecipientInstance.pauseWhileSet();

    return ExecutorFuture(**executor)
        .then([this, self = shared_from_this()] {
            stdx::lock_guard lk(_mutex);
            // Instance task can be started only once for the current term on a primary.
            invariant(!_taskState.isDone());
            // If the task state is interrupted, then don't start the task.
            if (_taskState.isInterrupted()) {
                uassertStatusOK(_taskState.getInterruptStatus());
            }

            _taskState.setState(TaskState::kRunning);

            pauseAfterRunTenantMigrationRecipientInstance.pauseWhileSet();

            return _initializeStateDoc(lk);
        })
        .then([this, self = shared_from_this()] {
            _stateDocPersistedPromise.emplaceValue();
            uassert(ErrorCodes::TenantMigrationForgotten,
                    str::stream() << "Migration " << getMigrationUUID()
                                  << " already marked for garbage collect",
                    !_stateDoc.getExpireAt());
            _stopOrHangOnFailPoint(&fpAfterPersistingTenantMigrationRecipientInstanceStateDoc);
            return _createAndConnectClients();
        })
        .then([this, self = shared_from_this()](ConnectionPair ConnectionPair) {
            stdx::lock_guard lk(_mutex);
            if (_taskState.isInterrupted()) {
                uassertStatusOK(_taskState.getInterruptStatus());
            }

            // interrupt() called after this code block will interrupt the cloner and fetcher.
            _client = std::move(ConnectionPair.first);
            _oplogFetcherClient = std::move(ConnectionPair.second);

            // Create the writer pool and shared data.
            _writerPool = makeTenantMigrationWriterPool();
            _sharedData = std::make_unique<TenantMigrationSharedData>(
                getGlobalServiceContext()->getFastClockSource(),
                getMigrationUUID(),
                _stateDoc.getStartFetchingDonorOpTime().has_value());
        })
        .then([this, self = shared_from_this(), token] {
            _fetchAndStoreDonorClusterTimeKeyDocs(token);
        })
        .then([this, self = shared_from_this()] {
            _stopOrHangOnFailPoint(&fpAfterConnectingTenantMigrationRecipientInstance);
            stdx::lock_guard lk(_mutex);

            // Record the FCV at the start of a migration and check for changes in every subsequent
            // attempt. Fail if there is any mismatch in FCV or upgrade/downgrade state.
            // (Generic FCV reference): This FCV check should exist across LTS binary versions.
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
        })
        .then([this, self = shared_from_this()] {
            _stopOrHangOnFailPoint(&fpAfterRecordingRecipientPrimaryStartingFCV);
            _compareRecipientAndDonorFCV();
        })
        .then([this, self = shared_from_this()] {
            _stopOrHangOnFailPoint(&fpAfterComparingRecipientAndDonorFCV);
            stdx::lock_guard lk(_mutex);
            _getStartOpTimesFromDonor(lk);
            return _updateStateDocForMajority(lk);
        })
        .then([this, self = shared_from_this()] { _fetchRetryableWritesOplogBeforeStartOpTime(); })
        .then([this, self = shared_from_this()] {
            _stopOrHangOnFailPoint(&fpAfterRetrievingStartOpTimesMigrationRecipientInstance);
            _startOplogFetcher();
        })
        .then([this, self = shared_from_this()] {
            _stopOrHangOnFailPoint(&fpAfterStartingOplogFetcherMigrationRecipientInstance);

            stdx::unique_lock lk(_mutex);

            {
                // Throwing error when cloner is canceled externally via interrupt(), makes the
                // instance to skip the remaining task (i.e., starting oplog applier) in the
                // sync process. This step is necessary to prevent race between interrupt()
                // and starting oplog applier for the failover scenarios where we don't start
                // the cloner if the tenant data is already in consistent state.
                stdx::lock_guard<TenantMigrationSharedData> sharedDatalk(*_sharedData);
                uassertStatusOK(_sharedData->getStatus(sharedDatalk));
            }

            // Create the oplog applier but do not start it yet.
            invariant(_stateDoc.getStartApplyingDonorOpTime());

            OpTime beginApplyingAfterOpTime;
            bool isResuming = false;
            if (_isCloneCompletedMarkerSet(lk)) {
                const auto startApplyingDonorOpTime = *_stateDoc.getStartApplyingDonorOpTime();
                const auto cloneFinishedRecipientOptime =
                    *_stateDoc.getCloneFinishedRecipientOpTime();
                lk.unlock();
                // We avoid holding the mutex while scanning the local oplog which acquires the RSTL
                // in IX mode. This is to allow us to be interruptable via a concurrent stepDown
                // which acquires the RSTL in X mode.
                beginApplyingAfterOpTime = _getOplogResumeApplyingDonorOptime(
                    startApplyingDonorOpTime, cloneFinishedRecipientOptime);
                isResuming = beginApplyingAfterOpTime > startApplyingDonorOpTime;
                lk.lock();
            } else {
                beginApplyingAfterOpTime = *_stateDoc.getStartApplyingDonorOpTime();
            }
            LOGV2_DEBUG(4881202,
                        1,
                        "Recipient migration service creating oplog applier",
                        "tenantId"_attr = getTenantId(),
                        "migrationId"_attr = getMigrationUUID(),
                        "startApplyingDonorOpTime"_attr = beginApplyingAfterOpTime);
            _tenantOplogApplier = std::make_shared<TenantOplogApplier>(_migrationUuid,
                                                                       _tenantId,
                                                                       beginApplyingAfterOpTime,
                                                                       _donorOplogBuffer.get(),
                                                                       **_scopedExecutor,
                                                                       _writerPool.get(),
                                                                       isResuming);

            // Start the cloner.
            auto clonerFuture = _startTenantAllDatabaseCloner(lk);

            // Signal that the data sync has started successfully.
            _dataSyncStartedPromise.emplaceValue();
            return clonerFuture;
        })
        .then([this, self = shared_from_this()] { return _onCloneSuccess(); })
        .then([this, self = shared_from_this()] { _fetchCommittedTransactionsBeforeStartOpTime(); })
        .then([this, self = shared_from_this()] {
            _stopOrHangOnFailPoint(&fpAfterCollectionClonerDone);
            LOGV2_DEBUG(4881200,
                        1,
                        "Recipient migration service starting oplog applier",
                        "tenantId"_attr = getTenantId(),
                        "migrationId"_attr = getMigrationUUID());
            {
                stdx::lock_guard lk(_mutex);
                uassertStatusOK(_tenantOplogApplier->startup());
            }
            _stopOrHangOnFailPoint(&fpAfterStartingOplogApplierMigrationRecipientInstance);
            return _getDataConsistentFuture();
        })
        .then([this, self = shared_from_this()] {
            _stopOrHangOnFailPoint(&fpBeforeFulfillingDataConsistentPromise);
            stdx::lock_guard lk(_mutex);
            LOGV2_DEBUG(4881101,
                        1,
                        "Tenant migration recipient instance is in consistent state",
                        "migrationId"_attr = getMigrationUUID(),
                        "tenantId"_attr = getTenantId(),
                        "donorConsistentOpTime"_attr =
                            _stateDoc.getDataConsistentStopDonorOpTime());

            _dataConsistentPromise.emplaceValue(_stateDoc.getDataConsistentStopDonorOpTime().get());
        })
        .then([this, self = shared_from_this()] {
            _stopOrHangOnFailPoint(&fpAfterDataConsistentMigrationRecipientInstance);
            stdx::lock_guard lk(_mutex);
            // wait for oplog applier to complete/stop.
            // The oplog applier does not exit normally; it must be shut down externally,
            // e.g. by recipientForgetMigration.
            return _tenantOplogApplier->getNotificationForOpTime(OpTime::max());
        })
        .thenRunOn(_recipientService->getInstanceCleanupExecutor())
        .onCompletion([this, self = shared_from_this()](
                          StatusOrStatusWith<TenantOplogApplier::OpTimePair> applierStatus) {
            // On shutDown/stepDown, the _scopedExecutor may have already been shut down. So we need
            // to schedule the clean up work on the parent executor.

            // We don't need the final optime from the oplog applier. The data sync does not
            // normally stop by itself on success. It completes only on errors or on external
            // interruption (e.g. by shutDown/stepDown or by recipientForgetMigration command).
            Status status = applierStatus.getStatus();

            // If we were interrupted during oplog application, replace oplog application
            // status with error state.
            // Network and cancellation errors can be caused due to interrupt() (which shuts
            // down the cloner/fetcher dbClientConnection & oplog applier), so replace those
            // error status with interrupt status, if set.
            if (ErrorCodes::isCancelationError(status) || ErrorCodes::isNetworkError(status)) {
                stdx::lock_guard lk(_mutex);
                if (_taskState.isInterrupted()) {
                    LOGV2(4881207,
                          "Migration completed with both error and interrupt",
                          "tenantId"_attr = getTenantId(),
                          "migrationId"_attr = getMigrationUUID(),
                          "completionStatus"_attr = status,
                          "interruptStatus"_attr = _taskState.getInterruptStatus());
                    status = _taskState.getInterruptStatus();
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
            if (_stateDoc.getExpireAt() ||
                MONGO_unlikely(autoRecipientForgetMigration.shouldFail())) {
                // Skip waiting for the recipientForgetMigration command.
                setPromiseOkifNotReady(lk, _receivedRecipientForgetMigrationPromise);
            }
        })
        .thenRunOn(**_scopedExecutor)
        .then([this, self = shared_from_this()] {
            // Schedule on the _scopedExecutor to make sure we are still the primary when waiting
            // for the recipientForgetMigration command.
            return _receivedRecipientForgetMigrationPromise.getFuture();
        })
        .then([this, self = shared_from_this()] { return _markStateDocAsGarbageCollectable(); })
        .then([this, self = shared_from_this()] {
            _stopOrHangOnFailPoint(&fpBeforeDroppingOplogBufferCollection);
            auto opCtx = cc().makeOperationContext();
            auto storageInterface = StorageInterface::get(opCtx.get());

            // The oplog buffer collection can be safely dropped at this point. In case it doesn't
            // exist, dropping will be a no-op.
            // It isn't necessary that the drop is majority-committed. A new primary will attempt
            // to drop the collection anyway.
            return storageInterface->dropCollection(opCtx.get(),
                                                    getOplogBufferNs(getMigrationUUID()));
        })
        .thenRunOn(_recipientService->getInstanceCleanupExecutor())
        .onCompletion([this, self = shared_from_this()](Status status) {
            // Schedule on the parent executor to mark the completion of the whole chain so this is
            // safe even on shutDown/stepDown.
            stdx::lock_guard lk(_mutex);
            invariant(_dataSyncCompletionPromise.getFuture().isReady());
            if (status.isOK()) {
                LOGV2(4881401,
                      "Migration marked to be garbage collectable due to recipientForgetMigration "
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

const UUID& TenantMigrationRecipientService::Instance::getMigrationUUID() const {
    return _migrationUuid;
}

const std::string& TenantMigrationRecipientService::Instance::getTenantId() const {
    return _tenantId;
}

}  // namespace repl
}  // namespace mongo
