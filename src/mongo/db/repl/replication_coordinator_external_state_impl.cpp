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

#include "mongo/base/init.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/rs_initialsync.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/snapshot_thread.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/s/balancer/balancer.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/listen.h"

namespace mongo {
namespace repl {

namespace {
const char configCollectionName[] = "local.system.replset";
const char configDatabaseName[] = "local";
const char lastVoteCollectionName[] = "local.replset.election";
const char lastVoteDatabaseName[] = "local";
const char meCollectionName[] = "local.me";
const char meDatabaseName[] = "local";
const char tsFieldName[] = "ts";

const char kCollectionOplogBufferName[] = "collection";
const char kBlockingQueueOplogBufferName[] = "inMemoryBlockingQueue";

// Set this to true to force background creation of snapshots even if --enableMajorityReadConcern
// isn't specified. This can be used for A-B benchmarking to find how much overhead
// repl::SnapshotThread introduces.
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(enableReplSnapshotThread, bool, false);

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(use3dot2InitialSync, bool, false);

// Set this to specify whether to use a collection to buffer the oplog on the destination server
// during initial sync to prevent rolling over the oplog.
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(initialSyncOplogBuffer,
                                      std::string,
                                      kBlockingQueueOplogBufferName);

MONGO_INITIALIZER(initialSyncOplogBuffer)(InitializerContext*) {
    if ((initialSyncOplogBuffer != kCollectionOplogBufferName) &&
        (initialSyncOplogBuffer != kBlockingQueueOplogBufferName)) {
        return Status(ErrorCodes::BadValue,
                      "unsupported initial sync oplog buffer option: " + initialSyncOplogBuffer);
    }
    if (use3dot2InitialSync && (initialSyncOplogBuffer == kCollectionOplogBufferName)) {
        return Status(ErrorCodes::BadValue,
                      "cannot use collection oplog buffer without --setParameter "
                      "use3dot2InitialSync=false");
    }

    return Status::OK();
}

/**
 * Returns new thread pool for thread pool task executor.
 */
std::unique_ptr<ThreadPool> makeThreadPool() {
    ThreadPool::Options threadPoolOptions;
    threadPoolOptions.poolName = "replication";
    threadPoolOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return stdx::make_unique<ThreadPool>(threadPoolOptions);
}

}  // namespace

ReplicationCoordinatorExternalStateImpl::ReplicationCoordinatorExternalStateImpl(
    StorageInterface* storageInterface)
    : _storageInterface(storageInterface) {
    uassert(ErrorCodes::BadValue, "A StorageInterface is required.", _storageInterface);
}
ReplicationCoordinatorExternalStateImpl::~ReplicationCoordinatorExternalStateImpl() {}

bool ReplicationCoordinatorExternalStateImpl::isInitialSyncFlagSet(OperationContext* txn) {
    return _storageInterface->getInitialSyncFlag(txn);
}

void ReplicationCoordinatorExternalStateImpl::startInitialSync(OnInitialSyncFinishedFn finished) {
    _initialSyncThread.reset(new stdx::thread{[finished, this]() {
        Client::initThreadIfNotAlready("initial sync");
        // Do initial sync.
        syncDoInitialSync(this);
        finished();
    }});
}

void ReplicationCoordinatorExternalStateImpl::runOnInitialSyncThread(
    stdx::function<void(OperationContext* txn)> run) {
    _initialSyncThread.reset(new stdx::thread{[run, this]() {
        Client::initThreadIfNotAlready("initial sync");
        auto txn = cc().makeOperationContext();
        invariant(txn);
        invariant(txn->getClient());
        run(txn.get());
    }});
}

void ReplicationCoordinatorExternalStateImpl::startSteadyStateReplication(OperationContext* txn) {
    invariant(!_bgSync);
    log() << "Starting replication fetcher thread";
    _bgSync = stdx::make_unique<BackgroundSync>(this, makeSteadyStateOplogBuffer(txn));
    _bgSync->startup(txn);

    log() << "Starting replication applier threads";
    invariant(!_applierThread);
    _applierThread.reset(new stdx::thread(stdx::bind(&runSyncThread, _bgSync.get())));
    log() << "Starting replication reporter thread";
    invariant(!_syncSourceFeedbackThread);
    _syncSourceFeedbackThread.reset(new stdx::thread(stdx::bind(
        &SyncSourceFeedback::run, &_syncSourceFeedback, _taskExecutor.get(), _bgSync.get())));
}

void ReplicationCoordinatorExternalStateImpl::startThreads(const ReplSettings& settings) {
    stdx::lock_guard<stdx::mutex> lk(_threadMutex);
    if (_startedThreads) {
        return;
    }
    log() << "Starting replication storage threads";
    if (settings.isMajorityReadConcernEnabled() || enableReplSnapshotThread) {
        _snapshotThread = SnapshotThread::start(getGlobalServiceContext());
    }
    getGlobalServiceContext()->getGlobalStorageEngine()->setJournalListener(this);

    _taskExecutor = stdx::make_unique<executor::ThreadPoolTaskExecutor>(
        makeThreadPool(), executor::makeNetworkInterface("NetworkInterfaceASIO-RS"));
    _taskExecutor->startup();

    _writerPool = SyncTail::makeWriterPool();

    _storageInterface->startup();

    _startedThreads = true;
}

void ReplicationCoordinatorExternalStateImpl::startMasterSlave(OperationContext* txn) {
    repl::startMasterSlave(txn);
}

void ReplicationCoordinatorExternalStateImpl::shutdown(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> lk(_threadMutex);
    if (_startedThreads) {
        log() << "Stopping replication applier threads";
        if (_syncSourceFeedbackThread) {
            _syncSourceFeedback.shutdown();
            _syncSourceFeedbackThread->join();
        }
        if (_applierThread) {
            _applierThread->join();
        }

        if (_bgSync) {
            _bgSync->shutdown(txn);
            _bgSync->join(txn);
        }
        if (_snapshotThread)
            _snapshotThread->shutdown();

        _taskExecutor->shutdown();
        _taskExecutor->join();
        _storageInterface->shutdown();
    }
}

executor::TaskExecutor* ReplicationCoordinatorExternalStateImpl::getTaskExecutor() const {
    return _taskExecutor.get();
}

OldThreadPool* ReplicationCoordinatorExternalStateImpl::getDbWorkThreadPool() const {
    return _writerPool.get();
}

Status ReplicationCoordinatorExternalStateImpl::initializeReplSetStorage(OperationContext* txn,
                                                                         const BSONObj& config) {
    try {
        createOplog(txn);

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ScopedTransaction scopedXact(txn, MODE_X);
            Lock::GlobalWrite globalWrite(txn->lockState());

            WriteUnitOfWork wuow(txn);
            Helpers::putSingleton(txn, configCollectionName, config);
            const auto msgObj = BSON("msg"
                                     << "initiating set");
            getGlobalServiceContext()->getOpObserver()->onOpMessage(txn, msgObj);
            wuow.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "initiate oplog entry", "local.oplog.rs");
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    return Status::OK();
}

void ReplicationCoordinatorExternalStateImpl::logTransitionToPrimaryToOplog(OperationContext* txn) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        ScopedTransaction scopedXact(txn, MODE_X);

        WriteUnitOfWork wuow(txn);
        txn->getClient()->getServiceContext()->getOpObserver()->onOpMessage(txn,
                                                                            BSON("msg"
                                                                                 << "new primary"));
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        txn, "logging transition to primary to oplog", "local.oplog.rs");
}

void ReplicationCoordinatorExternalStateImpl::forwardSlaveProgress() {
    _syncSourceFeedback.forwardSlaveProgress();
}

OID ReplicationCoordinatorExternalStateImpl::ensureMe(OperationContext* txn) {
    std::string myname = getHostName();
    OID myRID;
    {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock lock(txn->lockState(), meDatabaseName, MODE_X);

        BSONObj me;
        // local.me is an identifier for a server for getLastError w:2+
        // TODO: handle WriteConflictExceptions below
        if (!Helpers::getSingleton(txn, meCollectionName, me) || !me.hasField("host") ||
            me["host"].String() != myname) {
            myRID = OID::gen();

            // clean out local.me
            Helpers::emptyCollection(txn, meCollectionName);

            // repopulate
            BSONObjBuilder b;
            b.append("_id", myRID);
            b.append("host", myname);
            Helpers::putSingleton(txn, meCollectionName, b.done());
        } else {
            myRID = me["_id"].OID();
        }
    }
    return myRID;
}

StatusWith<BSONObj> ReplicationCoordinatorExternalStateImpl::loadLocalConfigDocument(
    OperationContext* txn) {
    try {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            BSONObj config;
            if (!Helpers::getSingleton(txn, configCollectionName, config)) {
                return StatusWith<BSONObj>(
                    ErrorCodes::NoMatchingDocument,
                    str::stream() << "Did not find replica set configuration document in "
                                  << configCollectionName);
            }
            return StatusWith<BSONObj>(config);
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "load replica set config", configCollectionName);
    } catch (const DBException& ex) {
        return StatusWith<BSONObj>(ex.toStatus());
    }
}

Status ReplicationCoordinatorExternalStateImpl::storeLocalConfigDocument(OperationContext* txn,
                                                                         const BSONObj& config) {
    try {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbWriteLock(txn->lockState(), configDatabaseName, MODE_X);
            Helpers::putSingleton(txn, configCollectionName, config);
            return Status::OK();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "save replica set config", configCollectionName);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<LastVote> ReplicationCoordinatorExternalStateImpl::loadLocalLastVoteDocument(
    OperationContext* txn) {
    try {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            BSONObj lastVoteObj;
            if (!Helpers::getSingleton(txn, lastVoteCollectionName, lastVoteObj)) {
                return StatusWith<LastVote>(ErrorCodes::NoMatchingDocument,
                                            str::stream()
                                                << "Did not find replica set lastVote document in "
                                                << lastVoteCollectionName);
            }
            LastVote lastVote;
            lastVote.initialize(lastVoteObj);
            return StatusWith<LastVote>(lastVote);
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
            txn, "load replica set lastVote", lastVoteCollectionName);
    } catch (const DBException& ex) {
        return StatusWith<LastVote>(ex.toStatus());
    }
}

Status ReplicationCoordinatorExternalStateImpl::storeLocalLastVoteDocument(
    OperationContext* txn, const LastVote& lastVote) {
    BSONObj lastVoteObj = lastVote.toBSON();
    try {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbWriteLock(txn->lockState(), lastVoteDatabaseName, MODE_X);
            Helpers::putSingleton(txn, lastVoteCollectionName, lastVoteObj);
            return Status::OK();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
            txn, "save replica set lastVote", lastVoteCollectionName);
        MONGO_UNREACHABLE;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void ReplicationCoordinatorExternalStateImpl::setGlobalTimestamp(const Timestamp& newTime) {
    setNewTimestamp(newTime);
}

void ReplicationCoordinatorExternalStateImpl::cleanUpLastApplyBatch(OperationContext* txn) {
    if (_storageInterface->getInitialSyncFlag(txn)) {
        return;  // Initial Sync will take over so no cleanup is needed.
    }
    auto mv = _storageInterface->getMinValid(txn);

    if (!mv.start.isNull()) {
        // If we are in the middle of a batch, and recoveringm then we need to truncate the oplog.
        LOG(2) << "Recovering from a failed apply batch, start:" << mv.start.toBSON();
        truncateOplogTo(txn, mv.start.getTimestamp());
    }
}

StatusWith<OpTime> ReplicationCoordinatorExternalStateImpl::loadLastOpTime(OperationContext* txn) {
    // TODO: handle WriteConflictExceptions below
    try {
        // If we are doing an initial sync do not read from the oplog.
        if (_storageInterface->getInitialSyncFlag(txn)) {
            return {ErrorCodes::InitialSyncFailure, "In the middle of an initial sync."};
        }

        BSONObj oplogEntry;
        if (!Helpers::getLast(txn, rsOplogName.c_str(), oplogEntry)) {
            return StatusWith<OpTime>(ErrorCodes::NoMatchingDocument,
                                      str::stream() << "Did not find any entries in "
                                                    << rsOplogName);
        }
        BSONElement tsElement = oplogEntry[tsFieldName];
        if (tsElement.eoo()) {
            return StatusWith<OpTime>(ErrorCodes::NoSuchKey,
                                      str::stream() << "Most recent entry in " << rsOplogName
                                                    << " missing \""
                                                    << tsFieldName
                                                    << "\" field");
        }
        if (tsElement.type() != bsonTimestamp) {
            return StatusWith<OpTime>(ErrorCodes::TypeMismatch,
                                      str::stream() << "Expected type of \"" << tsFieldName
                                                    << "\" in most recent "
                                                    << rsOplogName
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
    const OperationContext* txn) {
    return HostAndPort(txn->getClient()->clientAddress(true));
}

void ReplicationCoordinatorExternalStateImpl::closeConnections() {
    getGlobalServiceContext()->getTransportLayer()->endAllSessions(transport::Session::kKeepOpen);
}

void ReplicationCoordinatorExternalStateImpl::killAllUserOperations(OperationContext* txn) {
    ServiceContext* environment = txn->getServiceContext();
    environment->killAllUserOperations(txn, ErrorCodes::InterruptedDueToReplStateChange);
}

void ReplicationCoordinatorExternalStateImpl::shardingOnStepDownHook() {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        Balancer::get(getGlobalServiceContext())->stopThread();
    }

    ShardingState::get(getGlobalServiceContext())->clearCollectionMetadata();
}

void ReplicationCoordinatorExternalStateImpl::shardingOnDrainingStateHook(OperationContext* txn) {
    auto status = ShardingStateRecovery::recover(txn);

    if (ErrorCodes::isShutdownError(status.code())) {
        // Note: callers of this method don't expect exceptions, so throw only unexpected fatal
        // errors.
        return;
    }

    if (!status.isOK()) {
        fassertFailedWithStatus(40107, status);
    }

    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        status = Grid::get(txn)->catalogManager()->initializeConfigDatabaseIfNeeded(txn);
        if (!status.isOK() && status != ErrorCodes::AlreadyInitialized) {
            if (ErrorCodes::isShutdownError(status.code())) {
                // Don't fassert if we're mid-shutdown, let the shutdown happen gracefully.
                return;
            }

            fassertFailedWithStatus(40184,
                                    Status(status.code(),
                                           str::stream()
                                               << "Failed to initialize config database on config "
                                                  "server's first transition to primary"
                                               << causedBy(status)));
        }

        if (status != ErrorCodes::AlreadyInitialized) {
            // Load the clusterId into memory. Use local readConcern, since we can't use majority
            // readConcern in drain mode because the global lock prevents replication. This is
            // safe, since if the clusterId write is rolled back, any writes that depend on it will
            // also be rolled back.
            // Since we *just* wrote the cluster ID to the config.version document (via
            // ShardingCatalogManager::initializeConfigDatabaseIfNeeded), this should always
            // succeed.
            status = ClusterIdentityLoader::get(txn)->loadClusterId(
                txn, repl::ReadConcernLevel::kLocalReadConcern);

            if (ErrorCodes::isShutdownError(status.code())) {
                // Don't fassert if we're mid-shutdown, let the shutdown happen gracefully.
                return;
            }

            fassertStatusOK(40217, status);
        }

        // Free any leftover locks from previous instantiations
        auto distLockManager = Grid::get(txn)->catalogClient(txn)->getDistLockManager();
        distLockManager->unlockAll(txn, distLockManager->getProcessID());

        // If this is a config server node becoming a primary, start the balancer
        auto balancer = Balancer::get(txn);

        // We need to join the balancer here, because it might have been running at a previous time
        // when this node was a primary.
        balancer->joinThread();
        balancer->startThread(txn);
    } else if (ShardingState::get(txn)->enabled()) {
        const auto configsvrConnStr =
            Grid::get(txn)->shardRegistry()->getConfigShard()->getConnString();
        auto status = ShardingState::get(txn)->updateShardIdentityConfigString(
            txn, configsvrConnStr.toString());
        if (!status.isOK()) {
            warning() << "error encountered while trying to update config connection string to "
                      << configsvrConnStr << causedBy(status);
        }
    }

    // There is a slight chance that some stale metadata might have been loaded before the latest
    // optime has been recovered, so throw out everything that we have up to now
    ShardingState::get(txn)->clearCollectionMetadata();
}

void ReplicationCoordinatorExternalStateImpl::signalApplierToChooseNewSyncSource() {
    if (_bgSync) {
        _bgSync->clearSyncTarget();
    }
}

void ReplicationCoordinatorExternalStateImpl::signalApplierToCancelFetcher() {
    invariant(_bgSync);
    _bgSync->cancelFetcher();
}

void ReplicationCoordinatorExternalStateImpl::dropAllTempCollections(OperationContext* txn) {
    std::vector<std::string> dbNames;
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
    storageEngine->listDatabases(&dbNames);

    for (std::vector<std::string>::iterator it = dbNames.begin(); it != dbNames.end(); ++it) {
        // The local db is special because it isn't replicated. It is cleared at startup even on
        // replica set members.
        if (*it == "local")
            continue;
        LOG(2) << "Removing temporary collections from " << *it;
        Database* db = dbHolder().get(txn, *it);
        // Since we must be holding the global lock during this function, if listDatabases
        // returned this dbname, we should be able to get a reference to it - it can't have
        // been dropped.
        invariant(db);
        db->clearTmpCollections(txn);
    }
}

void ReplicationCoordinatorExternalStateImpl::dropAllSnapshots() {
    if (auto manager = getGlobalServiceContext()->getGlobalStorageEngine()->getSnapshotManager())
        manager->dropAllSnapshots();
}

void ReplicationCoordinatorExternalStateImpl::updateCommittedSnapshot(SnapshotName newCommitPoint) {
    auto manager = getGlobalServiceContext()->getGlobalStorageEngine()->getSnapshotManager();
    invariant(manager);  // This should never be called if there is no SnapshotManager.
    manager->setCommittedSnapshot(newCommitPoint);
}

void ReplicationCoordinatorExternalStateImpl::forceSnapshotCreation() {
    if (_snapshotThread)
        _snapshotThread->forceSnapshot();
}

bool ReplicationCoordinatorExternalStateImpl::snapshotsEnabled() const {
    return _snapshotThread != nullptr;
}

void ReplicationCoordinatorExternalStateImpl::notifyOplogMetadataWaiters() {
    signalOplogWaiters();
}

double ReplicationCoordinatorExternalStateImpl::getElectionTimeoutOffsetLimitFraction() const {
    return replElectionTimeoutOffsetLimitFraction;
}

bool ReplicationCoordinatorExternalStateImpl::isReadCommittedSupportedByStorageEngine(
    OperationContext* txn) const {
    auto storageEngine = txn->getServiceContext()->getGlobalStorageEngine();
    // This should never be called if the storage engine has not been initialized.
    invariant(storageEngine);
    return storageEngine->getSnapshotManager();
}

StatusWith<OpTime> ReplicationCoordinatorExternalStateImpl::multiApply(
    OperationContext* txn,
    MultiApplier::Operations ops,
    MultiApplier::ApplyOperationFn applyOperation) {
    return repl::multiApply(txn, _writerPool.get(), std::move(ops), applyOperation);
}

void ReplicationCoordinatorExternalStateImpl::multiSyncApply(MultiApplier::OperationPtrs* ops) {
    // SyncTail* argument is not used by repl::multiSyncApply().
    repl::multiSyncApply(ops, nullptr);
}

void ReplicationCoordinatorExternalStateImpl::multiInitialSyncApply(
    MultiApplier::OperationPtrs* ops, const HostAndPort& source) {

    // repl::multiInitialSyncApply uses SyncTail::shouldRetry() (and implicitly getMissingDoc())
    // to fetch missing documents during initial sync. Therefore, it is fine to construct SyncTail
    // with invalid BackgroundSync, MultiSyncApplyFunc and writerPool arguments because we will not
    // be accessing any SyncTail functionality that require these constructor parameters.
    SyncTail syncTail(nullptr, SyncTail::MultiSyncApplyFunc(), nullptr);
    syncTail.setHostname(source.toString());
    repl::multiInitialSyncApply(ops, &syncTail);
}

std::unique_ptr<OplogBuffer> ReplicationCoordinatorExternalStateImpl::makeInitialSyncOplogBuffer(
    OperationContext* txn) const {
    if (initialSyncOplogBuffer == kCollectionOplogBufferName) {
        return stdx::make_unique<OplogBufferCollection>(StorageInterface::get(txn));
    } else {
        return stdx::make_unique<OplogBufferBlockingQueue>();
    }
}

std::unique_ptr<OplogBuffer> ReplicationCoordinatorExternalStateImpl::makeSteadyStateOplogBuffer(
    OperationContext* txn) const {
    return stdx::make_unique<OplogBufferBlockingQueue>();
}

bool ReplicationCoordinatorExternalStateImpl::shouldUseDataReplicatorInitialSync() const {
    return !use3dot2InitialSync;
}

JournalListener::Token ReplicationCoordinatorExternalStateImpl::getToken() {
    return repl::getGlobalReplicationCoordinator()->getMyLastAppliedOpTime();
}

void ReplicationCoordinatorExternalStateImpl::onDurable(const JournalListener::Token& token) {
    repl::getGlobalReplicationCoordinator()->setMyLastDurableOpTimeForward(token);
}

}  // namespace repl
}  // namespace mongo
