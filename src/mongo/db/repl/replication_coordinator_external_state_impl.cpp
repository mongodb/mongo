/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_coordinator_external_state_impl.h"

#include <string>

#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/free_mon/free_mon_mongod.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/noop_writer.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/s/chunk_splitter.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {
namespace {

using UniqueLock = stdx::unique_lock<stdx::mutex>;
using LockGuard = stdx::lock_guard<stdx::mutex>;

const char localDbName[] = "local";
const char configCollectionName[] = "local.system.replset";
const auto configDatabaseName = localDbName;
const char lastVoteCollectionName[] = "local.replset.election";
const auto lastVoteDatabaseName = localDbName;
const char meCollectionName[] = "local.me";
const auto meDatabaseName = localDbName;
const char tsFieldName[] = "ts";

MONGO_FAIL_POINT_DEFINE(dropPendingCollectionReaperHang);

// Set this to specify maximum number of times the oplog fetcher will consecutively restart the
// oplog tailing query on non-cancellation errors.
MONGO_EXPORT_SERVER_PARAMETER(oplogFetcherMaxFetcherRestarts, int, 3)
    ->withValidator([](const int& potentialNewValue) {
        if (potentialNewValue < 0) {
            return Status(ErrorCodes::BadValue,
                          "oplogFetcherMaxFetcherRestarts must be nonnegative");
        }
        return Status::OK();
    });

// The count of items in the buffer
OplogBuffer::Counters bufferGauge;
ServerStatusMetricField<Counter64> displayBufferCount("repl.buffer.count", &bufferGauge.count);
// The size (bytes) of items in the buffer
ServerStatusMetricField<Counter64> displayBufferSize("repl.buffer.sizeBytes", &bufferGauge.size);
// The max size (bytes) of the buffer. If the buffer does not have a size constraint, this is
// set to 0.
ServerStatusMetricField<Counter64> displayBufferMaxSize("repl.buffer.maxSizeBytes",
                                                        &bufferGauge.maxSize);

class NoopOplogApplierObserver : public repl::OplogApplier::Observer {
public:
    void onBatchBegin(const repl::OplogApplier::Operations&) final {}
    void onBatchEnd(const StatusWith<repl::OpTime>&, const repl::OplogApplier::Operations&) final {}
    void onMissingDocumentsFetchedAndInserted(const std::vector<FetchInfo>&) final {}
} noopOplogApplierObserver;

/**
 * Returns new thread pool for thread pool task executor.
 */
auto makeThreadPool(const std::string& poolName) {
    ThreadPool::Options threadPoolOptions;
    threadPoolOptions.poolName = poolName;
    threadPoolOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
        AuthorizationSession::get(cc())->grantInternalAuthorization();
    };
    return stdx::make_unique<ThreadPool>(threadPoolOptions);
}

/**
 * Returns a new thread pool task executor.
 */
auto makeTaskExecutor(ServiceContext* service, const std::string& poolName) {
    auto hookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(stdx::make_unique<rpc::LogicalTimeMetadataHook>(service));
    return stdx::make_unique<executor::ThreadPoolTaskExecutor>(
        makeThreadPool(poolName),
        executor::makeNetworkInterface("RS", nullptr, std::move(hookList)));
}

/**
 * Schedules a task using the executor. This task is always run unless the task executor is shutting
 * down.
 */
void scheduleWork(executor::TaskExecutor* executor,
                  const executor::TaskExecutor::CallbackFn& work) {
    auto cbh = executor->scheduleWork([work](const executor::TaskExecutor::CallbackArgs& args) {
        if (args.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        work(args);
    });
    if (cbh == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(40460, cbh);
}

}  // namespace

ReplicationCoordinatorExternalStateImpl::ReplicationCoordinatorExternalStateImpl(
    ServiceContext* service,
    DropPendingCollectionReaper* dropPendingCollectionReaper,
    StorageInterface* storageInterface,
    ReplicationProcess* replicationProcess)
    : _service(service),
      _dropPendingCollectionReaper(dropPendingCollectionReaper),
      _storageInterface(storageInterface),
      _replicationProcess(replicationProcess) {
    uassert(ErrorCodes::BadValue, "A StorageInterface is required.", _storageInterface);
}
ReplicationCoordinatorExternalStateImpl::~ReplicationCoordinatorExternalStateImpl() {}

bool ReplicationCoordinatorExternalStateImpl::isInitialSyncFlagSet(OperationContext* opCtx) {
    return _replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx);
}

// This function acquires the LockManager locks on oplog, so it cannot be called while holding
// ReplicationCoordinatorImpl's mutex.
void ReplicationCoordinatorExternalStateImpl::startSteadyStateReplication(
    OperationContext* opCtx, ReplicationCoordinator* replCoord) {

    // Initialize the cached pointer to the oplog collection, for writing to the oplog.
    acquireOplogCollectionForLogging(opCtx);

    LockGuard lk(_threadMutex);

    // We've shut down the external state, don't start again.
    if (_inShutdown)
        return;

    invariant(replCoord);
    _oplogBuffer = std::make_unique<OplogBufferBlockingQueue>(&bufferGauge);

    // No need to log OplogBuffer::startup because the blocking queue implementation
    // does not start any threads or access the storage layer.
    _oplogBuffer->startup(opCtx);

    invariant(!_oplogApplier);

    // Using noop observer now that BackgroundSync no longer implements the OplogApplier::Observer
    // interface. During steady state replication, there is no need to log details on every batch
    // we apply (recovery); or track missing documents that are fetched from the sync source
    // (initial sync).
    _oplogApplier =
        stdx::make_unique<OplogApplierImpl>(_oplogApplierTaskExecutor.get(),
                                            _oplogBuffer.get(),
                                            &noopOplogApplierObserver,
                                            replCoord,
                                            _replicationProcess->getConsistencyMarkers(),
                                            _storageInterface,
                                            OplogApplier::Options(),
                                            _writerPool.get());

    invariant(!_bgSync);
    _bgSync =
        std::make_unique<BackgroundSync>(replCoord, this, _replicationProcess, _oplogApplier.get());

    log() << "Starting replication fetcher thread";
    _bgSync->startup(opCtx);

    log() << "Starting replication applier thread";
    _oplogApplierShutdownFuture = _oplogApplier->startup();

    log() << "Starting replication reporter thread";
    invariant(!_syncSourceFeedbackThread);
    // Get the pointer while holding the lock so that _stopDataReplication_inlock() won't
    // leave the unique pointer empty if the _syncSourceFeedbackThread's function starts
    // after _stopDataReplication_inlock's move.
    auto bgSyncPtr = _bgSync.get();
    _syncSourceFeedbackThread = stdx::make_unique<stdx::thread>([this, bgSyncPtr, replCoord] {
        _syncSourceFeedback.run(_taskExecutor.get(), bgSyncPtr, replCoord);
    });
}

void ReplicationCoordinatorExternalStateImpl::stopDataReplication(OperationContext* opCtx) {
    UniqueLock lk(_threadMutex);
    _stopDataReplication_inlock(opCtx, &lk);
}

void ReplicationCoordinatorExternalStateImpl::_stopDataReplication_inlock(OperationContext* opCtx,
                                                                          UniqueLock* lock) {
    // Make sue no other _stopDataReplication calls are in progress.
    _dataReplicationStopped.wait(*lock, [this]() { return !_stoppingDataReplication; });
    _stoppingDataReplication = true;

    auto oldSSF = std::move(_syncSourceFeedbackThread);
    auto oldOplogBuffer = std::move(_oplogBuffer);
    auto oldBgSync = std::move(_bgSync);
    auto oldApplier = std::move(_oplogApplier);
    lock->unlock();

    // _syncSourceFeedbackThread should be joined before _bgSync's shutdown because it has
    // a pointer of _bgSync.
    if (oldSSF) {
        log() << "Stopping replication reporter thread";
        _syncSourceFeedback.shutdown();
        oldSSF->join();
    }

    if (oldBgSync) {
        log() << "Stopping replication fetcher thread";
        oldBgSync->shutdown(opCtx);
    }

    if (oldApplier) {
        log() << "Stopping replication applier thread";
        oldApplier->shutdown();
    }

    // Clear the buffer. This unblocks the OplogFetcher if it is blocked with a full queue, but
    // ensures that it won't add anything. It will also unblock the OplogApplier pipeline if it is
    // waiting for an operation to be past the slaveDelay point.
    if (oldOplogBuffer) {
        oldOplogBuffer->clear(opCtx);
    }

    if (oldBgSync) {
        oldBgSync->join(opCtx);
    }

    if (oldApplier) {
        _oplogApplierShutdownFuture.get();
    }

    if (oldOplogBuffer) {
        oldOplogBuffer->shutdown(opCtx);
    }

    lock->lock();
    _stoppingDataReplication = false;
    _dataReplicationStopped.notify_all();
}


void ReplicationCoordinatorExternalStateImpl::startThreads(const ReplSettings& settings) {
    stdx::lock_guard<stdx::mutex> lk(_threadMutex);
    if (_startedThreads) {
        return;
    }

    log() << "Starting replication storage threads";
    _service->getStorageEngine()->setJournalListener(this);

    _oplogApplierTaskExecutor = makeTaskExecutor(_service, "rsSync");
    _oplogApplierTaskExecutor->startup();

    _taskExecutor = makeTaskExecutor(_service, "replication");
    _taskExecutor->startup();

    _writerPool = OplogApplier::makeWriterPool();

    _startedThreads = true;
}

void ReplicationCoordinatorExternalStateImpl::shutdown(OperationContext* opCtx) {
    UniqueLock lk(_threadMutex);
    if (!_startedThreads) {
        return;
    }

    _inShutdown = true;
    _stopDataReplication_inlock(opCtx, &lk);

    if (_noopWriter) {
        LOG(1) << "Stopping noop writer";
        _noopWriter->stopWritingPeriodicNoops();
    }

    log() << "Stopping replication storage threads";
    _taskExecutor->shutdown();
    _oplogApplierTaskExecutor->shutdown();

    _oplogApplierTaskExecutor->join();
    _taskExecutor->join();
    lk.unlock();

    // Perform additional shutdown steps below that must be done outside _threadMutex.

    if (_replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx).isNull() &&
        loadLastOpTime(opCtx) ==
            _replicationProcess->getConsistencyMarkers()->getAppliedThrough(opCtx)) {
        // Clear the appliedThrough marker to indicate we are consistent with the top of the
        // oplog. We record this update at the 'lastAppliedOpTime'. If there are any outstanding
        // checkpoints being taken, they should only reflect this write if they see all writes up
        // to our 'lastAppliedOpTime'.
        auto lastAppliedOpTime = repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime();
        _replicationProcess->getConsistencyMarkers()->clearAppliedThrough(
            opCtx, lastAppliedOpTime.getTimestamp());
    }
}

executor::TaskExecutor* ReplicationCoordinatorExternalStateImpl::getTaskExecutor() const {
    return _taskExecutor.get();
}

ThreadPool* ReplicationCoordinatorExternalStateImpl::getDbWorkThreadPool() const {
    return _writerPool.get();
}

Status ReplicationCoordinatorExternalStateImpl::runRepairOnLocalDB(OperationContext* opCtx) {
    try {
        Lock::GlobalWrite globalWrite(opCtx);
        StorageEngine* engine = getGlobalServiceContext()->getStorageEngine();

        if (!engine->isMmapV1()) {
            return Status::OK();
        }

        UnreplicatedWritesBlock uwb(opCtx);
        Status status = repairDatabase(opCtx, engine, localDbName, false, false);

        // Open database before returning
        DatabaseHolder::getDatabaseHolder().openDb(opCtx, localDbName);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    return Status::OK();
}

Status ReplicationCoordinatorExternalStateImpl::initializeReplSetStorage(OperationContext* opCtx,
                                                                         const BSONObj& config) {
    try {
        createOplog(opCtx);

        writeConflictRetry(opCtx,
                           "initiate oplog entry",
                           NamespaceString::kRsOplogNamespace.toString(),
                           [this, &opCtx, &config] {
                               Lock::GlobalWrite globalWrite(opCtx);

                               WriteUnitOfWork wuow(opCtx);
                               Helpers::putSingleton(opCtx, configCollectionName, config);
                               const auto msgObj = BSON("msg"
                                                        << "initiating set");
                               _service->getOpObserver()->onOpMessage(opCtx, msgObj);
                               wuow.commit();
                               // ReplSetTest assumes that immediately after the replSetInitiate
                               // command returns, it can allow other nodes to initial sync with no
                               // retries and they will succeed.  Unfortunately, initial sync will
                               // fail if it finds its sync source has an empty oplog.  Thus, we
                               // need to wait here until the seed document is visible in our oplog.
                               AutoGetCollection oplog(
                                   opCtx, NamespaceString::kRsOplogNamespace, MODE_IS);
                               waitForAllEarlierOplogWritesToBeVisible(opCtx);
                           });

        // Update unique index format version for all non-replicated collections. It is possible
        // for MongoDB to have a "clean startup", i.e., no non-local databases, but still have
        // unique indexes on collections in the local database. On clean startup,
        // setFeatureCompatibilityVersion (which updates the unique index format version of
        // collections) is not called, so any pre-existing collections are upgraded here. We exclude
        // ShardServers when updating indexes belonging to non-replicated collections on the primary
        // because ShardServers are started up by default with featureCompatibilityVersion 4.0, so
        // we don't want to update those indexes until the cluster's featureCompatibilityVersion is
        // explicitly set to 4.2 by config server. The below unique index update for non-replicated
        // collections only occurs on the primary; updates for unique indexes belonging to
        // non-replicated collections are done on secondaries during InitialSync. When the config
        // server sets the featureCompatibilityVersion to 4.2, the shard primary will update unique
        // indexes belonging to all the collections. One special case here is if a shard is already
        // in featureCompatibilityVersion 4.2 and a new node is started up with --shardsvr and added
        // to that shard, the new node will still start up with featureCompatibilityVersion 4.0 and
        // may need to have unique index version updated. Such indexes would be updated during
        // InitialSync because the new node is a secondary.
        // TODO(SERVER-34489) Add a check for latest FCV when upgrade/downgrade is ready.
        if (FeatureCompatibilityVersion::isCleanStartUp() &&
            serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
            auto updateStatus = updateNonReplicatedUniqueIndexes(opCtx);
            if (!updateStatus.isOK())
                return updateStatus;
        }
        FeatureCompatibilityVersion::setIfCleanStartup(opCtx, _storageInterface);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    return Status::OK();
}

void ReplicationCoordinatorExternalStateImpl::waitForAllEarlierOplogWritesToBeVisible(
    OperationContext* opCtx) {
    AutoGetCollection oplog(opCtx, NamespaceString::kRsOplogNamespace, MODE_IS);
    oplog.getCollection()->getRecordStore()->waitForAllEarlierOplogWritesToBeVisible(opCtx);
}

void ReplicationCoordinatorExternalStateImpl::onDrainComplete(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());

    // If this is a config server node becoming a primary, ensure the balancer is ready to start.
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        // We must ensure the balancer has stopped because it may still be in the process of
        // stopping if this node was previously primary.
        Balancer::get(opCtx)->waitForBalancerToStop();
    }
}

OpTime ReplicationCoordinatorExternalStateImpl::onTransitionToPrimary(OperationContext* opCtx,
                                                                      bool isV1ElectionProtocol) {
    invariant(opCtx->lockState()->isW());

    // Clear the appliedThrough marker so on startup we'll use the top of the oplog. This must be
    // done before we add anything to our oplog.
    // We record this update at the 'lastAppliedOpTime'. If there are any outstanding
    // checkpoints being taken, they should only reflect this write if they see all writes up
    // to our 'lastAppliedOpTime'.
    invariant(
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx).isNull());
    auto lastAppliedOpTime = repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime();
    _replicationProcess->getConsistencyMarkers()->clearAppliedThrough(
        opCtx, lastAppliedOpTime.getTimestamp());

    if (isV1ElectionProtocol) {
        writeConflictRetry(opCtx, "logging transition to primary to oplog", "local.oplog.rs", [&] {
            WriteUnitOfWork wuow(opCtx);
            opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                opCtx,
                BSON("msg"
                     << "new primary"));
            wuow.commit();
        });
    }
    const auto opTimeToReturn = fassert(28665, loadLastOpTime(opCtx));

    _shardingOnTransitionToPrimaryHook(opCtx);
    _dropAllTempCollections(opCtx);

    serverGlobalParams.validateFeaturesAsMaster.store(true);

    return opTimeToReturn;
}

void ReplicationCoordinatorExternalStateImpl::forwardSlaveProgress() {
    _syncSourceFeedback.forwardSlaveProgress();
}

StatusWith<BSONObj> ReplicationCoordinatorExternalStateImpl::loadLocalConfigDocument(
    OperationContext* opCtx) {
    try {
        return writeConflictRetry(opCtx, "load replica set config", configCollectionName, [opCtx] {
            BSONObj config;
            if (!Helpers::getSingleton(opCtx, configCollectionName, config)) {
                return StatusWith<BSONObj>(
                    ErrorCodes::NoMatchingDocument,
                    str::stream() << "Did not find replica set configuration document in "
                                  << configCollectionName);
            }
            return StatusWith<BSONObj>(config);
        });
    } catch (const DBException& ex) {
        return StatusWith<BSONObj>(ex.toStatus());
    }
}

Status ReplicationCoordinatorExternalStateImpl::storeLocalConfigDocument(OperationContext* opCtx,
                                                                         const BSONObj& config) {
    try {
        writeConflictRetry(opCtx, "save replica set config", configCollectionName, [&] {
            Lock::DBLock dbWriteLock(opCtx, configDatabaseName, MODE_X);
            Helpers::putSingleton(opCtx, configCollectionName, config);
        });

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<LastVote> ReplicationCoordinatorExternalStateImpl::loadLocalLastVoteDocument(
    OperationContext* opCtx) {
    try {
        return writeConflictRetry(
            opCtx, "load replica set lastVote", lastVoteCollectionName, [opCtx] {
                BSONObj lastVoteObj;
                if (!Helpers::getSingleton(opCtx, lastVoteCollectionName, lastVoteObj)) {
                    return StatusWith<LastVote>(
                        ErrorCodes::NoMatchingDocument,
                        str::stream() << "Did not find replica set lastVote document in "
                                      << lastVoteCollectionName);
                }
                return LastVote::readFromLastVote(lastVoteObj);
            });
    } catch (const DBException& ex) {
        return StatusWith<LastVote>(ex.toStatus());
    }
}

Status ReplicationCoordinatorExternalStateImpl::storeLocalLastVoteDocument(
    OperationContext* opCtx, const LastVote& lastVote) {
    BSONObj lastVoteObj = lastVote.toBSON();
    try {
        Status status =
            writeConflictRetry(opCtx, "save replica set lastVote", lastVoteCollectionName, [&] {
                // If we are casting a vote in a new election immediately after stepping down, we
                // don't want to have this process interrupted due to us stepping down, since we
                // want to be able to cast our vote for a new primary right away.
                UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                Lock::DBLock dbWriteLock(opCtx, lastVoteDatabaseName, MODE_X);

                // If there is no last vote document, we want to store one. Otherwise, we only want
                // to replace it if the new last vote document would have a higher term. We both
                // check the term of the current last vote document and insert the new document
                // under the DBLock to synchronize the two operations.
                BSONObj result;
                bool exists = Helpers::getSingleton(opCtx, lastVoteCollectionName, result);
                if (!exists) {
                    Helpers::putSingleton(opCtx, lastVoteCollectionName, lastVoteObj);
                } else {
                    StatusWith<LastVote> oldLastVoteDoc = LastVote::readFromLastVote(result);
                    if (!oldLastVoteDoc.isOK()) {
                        return oldLastVoteDoc.getStatus();
                    }
                    if (lastVote.getTerm() > oldLastVoteDoc.getValue().getTerm()) {
                        Helpers::putSingleton(opCtx, lastVoteCollectionName, lastVoteObj);
                    }
                }

                return Status::OK();
            });

        if (!status.isOK()) {
            return status;
        }

        opCtx->recoveryUnit()->waitUntilDurable();

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void ReplicationCoordinatorExternalStateImpl::setGlobalTimestamp(ServiceContext* ctx,
                                                                 const Timestamp& newTime) {
    setNewTimestamp(ctx, newTime);
}

StatusWith<OpTime> ReplicationCoordinatorExternalStateImpl::loadLastOpTime(
    OperationContext* opCtx) {
    // TODO: handle WriteConflictExceptions below
    try {
        // If we are doing an initial sync do not read from the oplog.
        if (_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx)) {
            return {ErrorCodes::InitialSyncFailure, "In the middle of an initial sync."};
        }

        BSONObj oplogEntry;

        if (!writeConflictRetry(
                opCtx, "Load last opTime", NamespaceString::kRsOplogNamespace.ns().c_str(), [&] {
                    return Helpers::getLast(
                        opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), oplogEntry);
                })) {
            return StatusWith<OpTime>(ErrorCodes::NoMatchingDocument,
                                      str::stream() << "Did not find any entries in "
                                                    << NamespaceString::kRsOplogNamespace.ns());
        }
        BSONElement tsElement = oplogEntry[tsFieldName];
        if (tsElement.eoo()) {
            return StatusWith<OpTime>(ErrorCodes::NoSuchKey,
                                      str::stream() << "Most recent entry in "
                                                    << NamespaceString::kRsOplogNamespace.ns()
                                                    << " missing \""
                                                    << tsFieldName
                                                    << "\" field");
        }
        if (tsElement.type() != bsonTimestamp) {
            return StatusWith<OpTime>(ErrorCodes::TypeMismatch,
                                      str::stream() << "Expected type of \"" << tsFieldName
                                                    << "\" in most recent "
                                                    << NamespaceString::kRsOplogNamespace.ns()
                                                    << " entry to have type Timestamp, but found "
                                                    << typeName(tsElement.type()));
        }
        return OpTime::parseFromOplogEntry(oplogEntry);
    } catch (const DBException& ex) {
        return StatusWith<OpTime>(ex.toStatus());
    }
}

bool ReplicationCoordinatorExternalStateImpl::isSelf(const HostAndPort& host, ServiceContext* ctx) {
    return repl::isSelf(host, ctx);
}

HostAndPort ReplicationCoordinatorExternalStateImpl::getClientHostAndPort(
    const OperationContext* opCtx) {
    return HostAndPort(opCtx->getClient()->clientAddress(true));
}

void ReplicationCoordinatorExternalStateImpl::closeConnections() {
    _service->getServiceEntryPoint()->endAllSessions(transport::Session::kKeepOpen);
}

void ReplicationCoordinatorExternalStateImpl::killAllUserOperations(OperationContext* opCtx) {
    ServiceContext* environment = opCtx->getServiceContext();
    environment->killAllUserOperations(opCtx, ErrorCodes::InterruptedDueToStepDown);

    // Destroy all stashed transaction resources, in order to release locks.
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsLocalKillTransactions(opCtx, matcherAllSessions);
}

void ReplicationCoordinatorExternalStateImpl::shardingOnStepDownHook() {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        Balancer::get(_service)->interruptBalancer();
    } else if (ShardingState::get(_service)->enabled()) {
        invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);
        ChunkSplitter::get(_service).onStepDown();
        CatalogCacheLoader::get(_service).onStepDown();
    }

    if (auto validator = LogicalTimeValidator::get(_service)) {
        auto opCtx = cc().getOperationContext();

        if (opCtx != nullptr) {
            validator->enableKeyGenerator(opCtx, false);
        } else {
            auto opCtxPtr = cc().makeOperationContext();
            validator->enableKeyGenerator(opCtxPtr.get(), false);
        }
    }
}

void ReplicationCoordinatorExternalStateImpl::_shardingOnTransitionToPrimaryHook(
    OperationContext* opCtx) {
    auto status = ShardingStateRecovery::recover(opCtx);

    if (ErrorCodes::isShutdownError(status.code())) {
        // Note: callers of this method don't expect exceptions, so throw only unexpected fatal
        // errors.
        return;
    }

    fassert(40107, status);

    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        status = ShardingCatalogManager::get(opCtx)->initializeConfigDatabaseIfNeeded(opCtx);
        if (!status.isOK() && status != ErrorCodes::AlreadyInitialized) {
            if (ErrorCodes::isShutdownError(status.code())) {
                // Don't fassert if we're mid-shutdown, let the shutdown happen gracefully.
                return;
            }

            fassertFailedWithStatus(
                40184,
                status.withContext("Failed to initialize config database on config server's "
                                   "first transition to primary"));
        }

        if (status.isOK()) {
            // Load the clusterId into memory. Use local readConcern, since we can't use majority
            // readConcern in drain mode because the global lock prevents replication. This is
            // safe, since if the clusterId write is rolled back, any writes that depend on it will
            // also be rolled back.
            // Since we *just* wrote the cluster ID to the config.version document (via
            // ShardingCatalogManager::initializeConfigDatabaseIfNeeded), this should always
            // succeed.
            status = ClusterIdentityLoader::get(opCtx)->loadClusterId(
                opCtx, repl::ReadConcernLevel::kLocalReadConcern);

            if (ErrorCodes::isShutdownError(status.code())) {
                // Don't fassert if we're mid-shutdown, let the shutdown happen gracefully.
                return;
            }

            fassert(40217, status);
        }

        // Free any leftover locks from previous instantiations.
        auto distLockManager = Grid::get(opCtx)->catalogClient()->getDistLockManager();
        distLockManager->unlockAll(opCtx, distLockManager->getProcessID());

        // If this is a config server node becoming a primary, start the balancer
        Balancer::get(opCtx)->initiateBalancer(opCtx);

        if (auto validator = LogicalTimeValidator::get(_service)) {
            validator->enableKeyGenerator(opCtx, true);
        }
    } else if (ShardingState::get(opCtx)->enabled()) {
        invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

        const auto configsvrConnStr =
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->getConnString();
        auto status = ShardingState::get(opCtx)->updateShardIdentityConfigString(
            opCtx, configsvrConnStr.toString());
        if (!status.isOK()) {
            warning() << "error encountered while trying to update config connection string to "
                      << configsvrConnStr << causedBy(status);
        }

        CatalogCacheLoader::get(_service).onStepUp();
        ChunkSplitter::get(_service).onStepUp();
    } else {  // unsharded
        if (auto validator = LogicalTimeValidator::get(_service)) {
            validator->enableKeyGenerator(opCtx, true);
        }
    }

    SessionCatalog::get(_service)->onStepUp(opCtx);

    notifyFreeMonitoringOnTransitionToPrimary();
}

void ReplicationCoordinatorExternalStateImpl::signalApplierToChooseNewSyncSource() {
    LockGuard lk(_threadMutex);
    if (_bgSync) {
        _bgSync->clearSyncTarget();
    }
}

void ReplicationCoordinatorExternalStateImpl::stopProducer() {
    LockGuard lk(_threadMutex);
    if (_bgSync) {
        _bgSync->stop(false);
    }
}

void ReplicationCoordinatorExternalStateImpl::startProducerIfStopped() {
    LockGuard lk(_threadMutex);
    if (_bgSync) {
        _bgSync->startProducerIfStopped();
    }
}

void ReplicationCoordinatorExternalStateImpl::_dropAllTempCollections(OperationContext* opCtx) {
    std::vector<std::string> dbNames;
    StorageEngine* storageEngine = _service->getStorageEngine();
    storageEngine->listDatabases(&dbNames);

    for (std::vector<std::string>::iterator it = dbNames.begin(); it != dbNames.end(); ++it) {
        // The local db is special because it isn't replicated. It is cleared at startup even on
        // replica set members.
        if (*it == "local")
            continue;
        LOG(2) << "Removing temporary collections from " << *it;
        Database* db = DatabaseHolder::getDatabaseHolder().get(opCtx, *it);
        // Since we must be holding the global lock during this function, if listDatabases
        // returned this dbname, we should be able to get a reference to it - it can't have
        // been dropped.
        invariant(db, str::stream() << "Unable to get reference to database " << *it);
        db->clearTmpCollections(opCtx);
    }
}

void ReplicationCoordinatorExternalStateImpl::dropAllSnapshots() {
    if (auto manager = _service->getStorageEngine()->getSnapshotManager())
        manager->dropAllSnapshots();
}

void ReplicationCoordinatorExternalStateImpl::updateCommittedSnapshot(
    const OpTime& newCommitPoint) {
    auto manager = _service->getStorageEngine()->getSnapshotManager();
    if (manager) {
        manager->setCommittedSnapshot(newCommitPoint.getTimestamp());
    }
    notifyOplogMetadataWaiters(newCommitPoint);
}

void ReplicationCoordinatorExternalStateImpl::updateLocalSnapshot(const OpTime& optime) {
    auto manager = _service->getStorageEngine()->getSnapshotManager();
    if (manager) {
        manager->setLocalSnapshot(optime.getTimestamp());
    }
}

bool ReplicationCoordinatorExternalStateImpl::snapshotsEnabled() const {
    return _service->getStorageEngine()->getSnapshotManager() != nullptr;
}

void ReplicationCoordinatorExternalStateImpl::notifyOplogMetadataWaiters(
    const OpTime& committedOpTime) {
    signalOplogWaiters();

    // Notify the DropPendingCollectionReaper if there are any drop-pending collections with drop
    // optimes before or at the committed optime.
    if (auto earliestDropOpTime = _dropPendingCollectionReaper->getEarliestDropOpTime()) {
        if (committedOpTime >= *earliestDropOpTime) {
            auto reaper = _dropPendingCollectionReaper;
            scheduleWork(
                _taskExecutor.get(),
                [committedOpTime, reaper](const executor::TaskExecutor::CallbackArgs& args) {
                    if (MONGO_FAIL_POINT(dropPendingCollectionReaperHang)) {
                        log() << "fail point dropPendingCollectionReaperHang enabled. "
                                 "Blocking until fail point is disabled. "
                                 "committedOpTime: "
                              << committedOpTime;
                        MONGO_FAIL_POINT_PAUSE_WHILE_SET(dropPendingCollectionReaperHang);
                    }
                    auto opCtx = cc().makeOperationContext();
                    reaper->dropCollectionsOlderThan(opCtx.get(), committedOpTime);
                    auto replCoord = ReplicationCoordinator::get(opCtx.get());
                    replCoord->signalDropPendingCollectionsRemovedFromStorage();
                });
        }
    }
}

boost::optional<OpTime> ReplicationCoordinatorExternalStateImpl::getEarliestDropPendingOpTime()
    const {
    return _dropPendingCollectionReaper->getEarliestDropOpTime();
}

double ReplicationCoordinatorExternalStateImpl::getElectionTimeoutOffsetLimitFraction() const {
    return replElectionTimeoutOffsetLimitFraction;
}

bool ReplicationCoordinatorExternalStateImpl::isReadCommittedSupportedByStorageEngine(
    OperationContext* opCtx) const {
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    // This should never be called if the storage engine has not been initialized.
    invariant(storageEngine);
    return storageEngine->getSnapshotManager();
}

bool ReplicationCoordinatorExternalStateImpl::isReadConcernSnapshotSupportedByStorageEngine(
    OperationContext* opCtx) const {
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    // This should never be called if the storage engine has not been initialized.
    invariant(storageEngine);
    return storageEngine->supportsReadConcernSnapshot();
}

std::size_t ReplicationCoordinatorExternalStateImpl::getOplogFetcherMaxFetcherRestarts() const {
    return oplogFetcherMaxFetcherRestarts.load();
}

JournalListener::Token ReplicationCoordinatorExternalStateImpl::getToken() {
    return repl::ReplicationCoordinator::get(_service)->getMyLastAppliedOpTime();
}

void ReplicationCoordinatorExternalStateImpl::onDurable(const JournalListener::Token& token) {
    repl::ReplicationCoordinator::get(_service)->setMyLastDurableOpTimeForward(token);
}

void ReplicationCoordinatorExternalStateImpl::startNoopWriter(OpTime opTime) {
    invariant(_noopWriter);
    _noopWriter->startWritingPeriodicNoops(opTime).transitional_ignore();
}

void ReplicationCoordinatorExternalStateImpl::stopNoopWriter() {
    invariant(_noopWriter);
    _noopWriter->stopWritingPeriodicNoops();
}

void ReplicationCoordinatorExternalStateImpl::setupNoopWriter(Seconds waitTime) {
    invariant(!_noopWriter);

    _noopWriter = stdx::make_unique<NoopWriter>(waitTime);
}
}  // namespace repl
}  // namespace mongo
