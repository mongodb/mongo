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
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/snapshot_thread.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/network_interface.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/message_port.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/scopeguard.h"

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

void ReplicationCoordinatorExternalStateImpl::startThreads(const ReplSettings& settings) {
    stdx::lock_guard<stdx::mutex> lk(_threadMutex);
    if (_startedThreads) {
        return;
    }
    log() << "Starting replication applier threads";
    _applierThread.reset(new stdx::thread(runSyncThread));
    BackgroundSync* bgsync = BackgroundSync::get();
    _producerThread.reset(new stdx::thread(stdx::bind(&BackgroundSync::producerThread, bgsync)));
    _syncSourceFeedbackThread.reset(
        new stdx::thread(stdx::bind(&SyncSourceFeedback::run, &_syncSourceFeedback)));
    if (settings.isMajorityReadConcernEnabled() || enableReplSnapshotThread) {
        _snapshotThread = SnapshotThread::start(getGlobalServiceContext());
    }
    _startedThreads = true;
    getGlobalServiceContext()->getGlobalStorageEngine()->setJournalListener(this);
}

void ReplicationCoordinatorExternalStateImpl::startMasterSlave(OperationContext* txn) {
    repl::startMasterSlave(txn);
}

void ReplicationCoordinatorExternalStateImpl::shutdown(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> lk(_threadMutex);
    if (_startedThreads) {
        log() << "Stopping replication applier threads";
        _syncSourceFeedback.shutdown();
        _syncSourceFeedbackThread->join();
        _applierThread->join();
        BackgroundSync* bgsync = BackgroundSync::get();
        bgsync->shutdown();
        _producerThread->join();

        if (_snapshotThread)
            _snapshotThread->shutdown();

        if (getOplogDeleteFromPoint(txn).isNull() &&
            loadLastOpTime(txn) == getAppliedThrough(txn)) {
            // Clear the appliedThrough marker to indicate we are consistent with the top of the
            // oplog.
            setAppliedThrough(txn, {});
        }
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

        // This initializes the minvalid document with a null "ts" because older versions (<=3.2)
        // get angry if the minValid document is present but doesn't have a "ts" field.
        // Consider removing this once we no longer need to support downgrading to 3.2.
        setMinValidToAtLeast(txn, {});
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    return Status::OK();
}

OpTime ReplicationCoordinatorExternalStateImpl::onTransitionToPrimary(OperationContext* txn,
                                                                      bool isV1ElectionProtocol) {
    invariant(txn->lockState()->isW());

    // Clear the appliedThrough marker so on startup we'll use the top of the oplog. This must be
    // done before we add anything to our oplog.
    invariant(getOplogDeleteFromPoint(txn).isNull());
    setAppliedThrough(txn, {});

    if (isV1ElectionProtocol) {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ScopedTransaction scopedXact(txn, MODE_X);

            WriteUnitOfWork wuow(txn);
            txn->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                txn,
                BSON("msg"
                     << "new primary"));
            wuow.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
            txn, "logging transition to primary to oplog", "local.oplog.rs");
    }
    const auto opTimeToReturn = fassertStatusOK(28665, loadLastOpTime(txn));

    dropAllTempCollections(txn);

    return opTimeToReturn;
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
            return LastVote::readFromLastVote(lastVoteObj);
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

            // If there is no last vote document, we want to store one. Otherwise, we only want to
            // replace it if the new last vote document would have a higher term. We both check
            // the term of the current last vote document and insert the new document under the
            // DBLock to synchronize the two operations.
            BSONObj result;
            bool exists = Helpers::getSingleton(txn, lastVoteCollectionName, result);
            if (!exists) {
                Helpers::putSingleton(txn, lastVoteCollectionName, lastVoteObj);
            } else {
                StatusWith<LastVote> oldLastVoteDoc = LastVote::readFromLastVote(result);
                if (!oldLastVoteDoc.isOK()) {
                    return oldLastVoteDoc.getStatus();
                }
                if (lastVote.getTerm() > oldLastVoteDoc.getValue().getTerm()) {
                    Helpers::putSingleton(txn, lastVoteCollectionName, lastVoteObj);
                }
            }
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
            txn, "save replica set lastVote", lastVoteCollectionName);
        txn->recoveryUnit()->waitUntilDurable();
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void ReplicationCoordinatorExternalStateImpl::setGlobalTimestamp(const Timestamp& newTime) {
    setNewTimestamp(newTime);
}

void ReplicationCoordinatorExternalStateImpl::cleanUpLastApplyBatch(OperationContext* txn) {
    if (getInitialSyncFlag(txn)) {
        return;  // Initial Sync will take over so no cleanup is needed.
    }

    // This initializes the minvalid document with a null "ts" because older versions (<3.2.10)
    // get angry if the minValid document is present but doesn't have a "ts" field.
    setMinValidToAtLeast(txn, {});

    const auto deleteFromPoint = getOplogDeleteFromPoint(txn);
    const auto appliedThrough = getAppliedThrough(txn);

    const bool needToDeleteEndOfOplog = !deleteFromPoint.isNull() &&
        // This version should never have a non-null deleteFromPoint with a null appliedThrough.
        // This scenario means that we downgraded after unclean shutdown, then the downgraded node
        // deleted the ragged end of our oplog, then did a clean shutdown.
        !appliedThrough.isNull() &&
        // Similarly we should never have an appliedThrough higher than the deleteFromPoint. This
        // means that the downgraded node deleted our ragged end then applied ahead of our
        // deleteFromPoint and then had an unclean shutdown before upgrading. We are ok with
        // applying these ops because older versions wrote to the oplog from a single thread so we
        // know they are in order.
        !(appliedThrough.getTimestamp() >= deleteFromPoint);
    if (needToDeleteEndOfOplog) {
        log() << "Removing unapplied entries starting at: " << deleteFromPoint;
        truncateOplogTo(txn, deleteFromPoint);
    }
    setOplogDeleteFromPoint(txn, {});  // clear the deleteFromPoint

    if (appliedThrough.isNull()) {
        // No follow-up work to do.
        return;
    }

    // Check if we have any unapplied ops in our oplog. It is important that this is done after
    // deleting the ragged end of the oplog.
    const auto topOfOplog = fassertStatusOK(40290, loadLastOpTime(txn));
    if (appliedThrough == topOfOplog) {
        return;  // We've applied all the valid oplog we have.
    } else if (appliedThrough > topOfOplog) {
        severe() << "Applied op " << appliedThrough << " not found. Top of oplog is " << topOfOplog
                 << '.';
        fassertFailedNoTrace(40313);
    }

    log() << "Replaying stored operations from " << appliedThrough << " (exclusive) to "
          << topOfOplog << " (inclusive).";

    DBDirectClient db(txn);
    auto cursor = db.query(rsOplogName,
                           QUERY("ts" << BSON("$gte" << appliedThrough.getTimestamp())),
                           /*batchSize*/ 0,
                           /*skip*/ 0,
                           /*projection*/ nullptr,
                           QueryOption_OplogReplay);

    // Check that the first document matches our appliedThrough point then skip it since it's
    // already been applied.
    if (!cursor->more()) {
        // This should really be impossible because we check above that the top of the oplog is
        // strictly > appliedThrough. If this fails it represents a serious bug in either the
        // storage engine or query's implementation of OplogReplay.
        severe() << "Couldn't find any entries in the oplog >= " << appliedThrough
                 << " which should be impossible.";
        fassertFailedNoTrace(40293);
    }
    auto firstOpTimeFound = fassertStatusOK(40291, OpTime::parseFromOplogEntry(cursor->nextSafe()));
    if (firstOpTimeFound != appliedThrough) {
        severe() << "Oplog entry at " << appliedThrough << " is missing; actual entry found is "
                 << firstOpTimeFound;
        fassertFailedNoTrace(40292);
    }

    // Apply remaining ops one at at time, but don't log them because they are already logged.
    const bool wereWritesReplicated = txn->writesAreReplicated();
    ON_BLOCK_EXIT([&] { txn->setReplicatedWrites(wereWritesReplicated); });
    txn->setReplicatedWrites(false);

    while (cursor->more()) {
        auto entry = cursor->nextSafe();
        fassertStatusOK(40294, SyncTail::syncApply(txn, entry, true));
        setAppliedThrough(txn, fassertStatusOK(40295, OpTime::parseFromOplogEntry(entry)));
    }
}

StatusWith<OpTime> ReplicationCoordinatorExternalStateImpl::loadLastOpTime(OperationContext* txn) {
    // TODO: handle WriteConflictExceptions below
    try {
        // If we are doing an initial sync do not read from the oplog.
        if (getInitialSyncFlag(txn)) {
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
    MessagingPort::closeAllSockets(executor::NetworkInterface::kMessagingPortKeepOpen);
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

void ReplicationCoordinatorExternalStateImpl::signalApplierToChooseNewSyncSource() {
    BackgroundSync::get()->clearSyncTarget();
}

void ReplicationCoordinatorExternalStateImpl::signalApplierToCancelFetcher() {
    BackgroundSync::get()->cancelFetcher();
}

OperationContext* ReplicationCoordinatorExternalStateImpl::createOperationContext(
    const std::string& threadName) {
    Client::initThreadIfNotAlready(threadName.c_str());
    return new OperationContextImpl();
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

void ReplicationCoordinatorExternalStateImpl::createSnapshot(OperationContext* txn,
                                                             SnapshotName name) {
    auto manager = getGlobalServiceContext()->getGlobalStorageEngine()->getSnapshotManager();
    invariant(manager);  // This should never be called if there is no SnapshotManager.
    manager->createSnapshot(txn, name);
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

JournalListener::Token ReplicationCoordinatorExternalStateImpl::getToken() {
    return repl::getGlobalReplicationCoordinator()->getMyLastAppliedOpTime();
}

void ReplicationCoordinatorExternalStateImpl::onDurable(const JournalListener::Token& token) {
    repl::getGlobalReplicationCoordinator()->setMyLastDurableOpTimeForward(token);
}

}  // namespace repl
}  // namespace mongo
