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

#include <sstream>
#include <string>

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
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/rs_initialsync.h"
#include "mongo/db/repl/snapshot_thread.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/network_interface.h"
#include "mongo/s/grid.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
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

// Set this to true to force background creation of snapshots even if --enableMajorityReadConcern
// isn't specified. This can be used for A-B benchmarking to find how much overhead
// repl::SnapshotThread introduces.
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(enableReplSnapshotThread, bool, false);

}  // namespace

ReplicationCoordinatorExternalStateImpl::ReplicationCoordinatorExternalStateImpl()
    : _startedThreads(false), _nextThreadId(0) {}
ReplicationCoordinatorExternalStateImpl::~ReplicationCoordinatorExternalStateImpl() {}

void ReplicationCoordinatorExternalStateImpl::startInitialSync(OnInitialSyncFinishedFn finished) {
    _initialSyncThread.reset(new stdx::thread{[finished, this]() {
        Client::initThreadIfNotAlready("initial sync");
        log() << "Starting replication fetcher thread";

        // Start bgsync.
        BackgroundSync* bgsync = BackgroundSync::get();
        invariant(!(bgsync == nullptr && !inShutdownStrict()));  // bgsync can be null @shutdown.
        invariant(!_producerThread);  // The producer thread should not be init'd before this.
        _producerThread.reset(
            new stdx::thread(stdx::bind(&BackgroundSync::producerThread, bgsync, this)));
        // Do initial sync.
        syncDoInitialSync();
        finished();
    }});
}

void ReplicationCoordinatorExternalStateImpl::startSteadyStateReplication() {
    if (!_producerThread) {
        log() << "Starting replication fetcher thread";
        BackgroundSync* bgsync = BackgroundSync::get();
        _producerThread.reset(
            new stdx::thread(stdx::bind(&BackgroundSync::producerThread, bgsync, this)));
    }
    log() << "Starting replication applier threads";
    invariant(!_applierThread);
    _applierThread.reset(new stdx::thread(runSyncThread));
    log() << "Starting replication reporter thread";
    invariant(!_syncSourceFeedbackThread);
    _syncSourceFeedbackThread.reset(
        new stdx::thread(stdx::bind(&SyncSourceFeedback::run, &_syncSourceFeedback)));
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
    _startedThreads = true;
}

void ReplicationCoordinatorExternalStateImpl::startMasterSlave(OperationContext* txn) {
    repl::startMasterSlave(txn);
}

void ReplicationCoordinatorExternalStateImpl::shutdown() {
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

        if (_producerThread) {
            BackgroundSync* bgsync = BackgroundSync::get();
            if (bgsync) {
                bgsync->shutdown();
            }
            _producerThread->join();
        }
        if (_snapshotThread)
            _snapshotThread->shutdown();
    }
}

Status ReplicationCoordinatorExternalStateImpl::initializeReplSetStorage(OperationContext* txn,
                                                                         const BSONObj& config,
                                                                         bool updateReplOpTime) {
    try {
        createOplog(txn, rsOplogName, true);

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ScopedTransaction scopedXact(txn, MODE_X);
            Lock::GlobalWrite globalWrite(txn->lockState());

            WriteUnitOfWork wuow(txn);
            Helpers::putSingleton(txn, configCollectionName, config);
            const auto msgObj = BSON("msg"
                                     << "initiating set");
            if (updateReplOpTime) {
                getGlobalServiceContext()->getOpObserver()->onOpMessage(txn, msgObj);
            } else {
                // 'updateReplOpTime' is false when called from the replSetInitiate command when the
                // server is running with replication disabled. We bypass onOpMessage to invoke
                // _logOp directly so that we can override the replication mode and keep _logO from
                // updating the replication coordinator's op time (illegal operation when
                // replication is not enabled).
                repl::oplogCheckCloseDatabase(txn, nullptr);
                repl::_logOp(txn,
                             "n",
                             "",
                             msgObj,
                             nullptr,
                             false,
                             rsOplogName,
                             ReplicationCoordinator::modeReplSet,
                             updateReplOpTime);
                repl::oplogCheckCloseDatabase(txn, nullptr);
            }
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
    auto mv = StorageInterface::get(txn)->getMinValid(txn);

    if (!mv.start.isNull()) {
        // If we are in the middle of a batch, and recoveringm then we need to truncate the oplog.
        LOG(2) << "Recovering from a failed apply batch, start:" << mv.start.toBSON();
        truncateOplogTo(txn, mv.start.getTimestamp());
    }
}

StatusWith<OpTime> ReplicationCoordinatorExternalStateImpl::loadLastOpTime(OperationContext* txn) {
    // TODO: handle WriteConflictExceptions below
    try {
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
                                                    << " missing \"" << tsFieldName << "\" field");
        }
        if (tsElement.type() != bsonTimestamp) {
            return StatusWith<OpTime>(ErrorCodes::TypeMismatch,
                                      str::stream() << "Expected type of \"" << tsFieldName
                                                    << "\" in most recent " << rsOplogName
                                                    << " entry to have type Timestamp, but found "
                                                    << typeName(tsElement.type()));
        }
        return OpTime::parseFromOplogEntry(oplogEntry);
    } catch (const DBException& ex) {
        return StatusWith<OpTime>(ex.toStatus());
    }
}

bool ReplicationCoordinatorExternalStateImpl::isSelf(const HostAndPort& host) {
    return repl::isSelf(host);
}

HostAndPort ReplicationCoordinatorExternalStateImpl::getClientHostAndPort(
    const OperationContext* txn) {
    return HostAndPort(txn->getClient()->clientAddress(true));
}

void ReplicationCoordinatorExternalStateImpl::closeConnections() {
    Listener::closeMessagingPorts(executor::NetworkInterface::kMessagingPortKeepOpen);
}

void ReplicationCoordinatorExternalStateImpl::killAllUserOperations(OperationContext* txn) {
    ServiceContext* environment = txn->getServiceContext();
    environment->killAllUserOperations(txn, ErrorCodes::InterruptedDueToReplStateChange);
}

void ReplicationCoordinatorExternalStateImpl::clearShardingState() {
    ShardingState::get(getGlobalServiceContext())->clearCollectionMetadata();
}

void ReplicationCoordinatorExternalStateImpl::recoverShardingState(OperationContext* txn) {
    auto status = ShardingStateRecovery::recover(txn);

    if (status == ErrorCodes::ShutdownInProgress) {
        // Note: callers of this method don't expect exceptions, so throw only unexpected fatal
        // errors.
        return;
    }

    if (!status.isOK()) {
        fassertFailedWithStatus(40107, status);
    }

    // There is a slight chance that some stale metadata might have been loaded before the latest
    // optime has been recovered, so throw out everything that we have up to now
    ShardingState::get(txn)->clearCollectionMetadata();
}

void ReplicationCoordinatorExternalStateImpl::updateShardIdentityConfigString(
    OperationContext* txn) {
    if (ShardingState::get(txn)->enabled()) {
        const auto configsvrConnStr =
            Grid::get(txn)->shardRegistry()->getConfigShard()->getConnString();
        auto status = ShardingState::get(txn)
                          ->updateShardIdentityConfigString(txn, configsvrConnStr.toString());
        if (!status.isOK()) {
            warning() << "error encountered while trying to update config connection string to "
                      << configsvrConnStr << causedBy(status);
        }
    }
}

void ReplicationCoordinatorExternalStateImpl::signalApplierToChooseNewSyncSource() {
    auto bgsync = BackgroundSync::get();
    if (bgsync) {
        bgsync->clearSyncTarget();
    }
}

void ReplicationCoordinatorExternalStateImpl::signalApplierToCancelFetcher() {
    auto bgsync = BackgroundSync::get();
    if (bgsync) {
        bgsync->cancelFetcher();
    }
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
    const MultiApplier::Operations& ops,
    MultiApplier::ApplyOperationFn applyOperation) {
    return repl::multiApply(txn, ops, applyOperation);
}

void ReplicationCoordinatorExternalStateImpl::multiSyncApply(const MultiApplier::Operations& ops) {
    // SyncTail* argument is not used by repl::multiSyncApply().
    repl::multiSyncApply(ops, nullptr);
}

void ReplicationCoordinatorExternalStateImpl::multiInitialSyncApply(
    const MultiApplier::Operations& ops, const HostAndPort& source) {
    // repl::multiInitialSyncApply uses SyncTail::shouldRetry() (and implicitly getMissingDoc())
    // to fetch missing documents during initial sync. Therefore, it is fine to construct SyncTail
    // with invalid BackgroundSync and MultiSyncApplyFunc arguments because we will not be accessing
    // any SyncTail functionality that require these constructor parameters.
    SyncTail syncTail(nullptr, SyncTail::MultiSyncApplyFunc());
    syncTail.setHostname(source.toString());
    repl::multiInitialSyncApply(ops, &syncTail);
}


JournalListener::Token ReplicationCoordinatorExternalStateImpl::getToken() {
    return repl::getGlobalReplicationCoordinator()->getMyLastAppliedOpTime();
}

void ReplicationCoordinatorExternalStateImpl::onDurable(const JournalListener::Token& token) {
    repl::getGlobalReplicationCoordinator()->setMyLastDurableOpTimeForward(token);
}

}  // namespace repl
}  // namespace mongo
