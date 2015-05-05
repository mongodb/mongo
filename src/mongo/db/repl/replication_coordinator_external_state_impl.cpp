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

#include <boost/thread.hpp>
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
#include "mongo/db/service_context.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/s/d_state.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/message_port.h"
#include "mongo/util/net/sock.h"

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
}  // namespace

    ReplicationCoordinatorExternalStateImpl::ReplicationCoordinatorExternalStateImpl() :
        _startedThreads(false)
        , _nextThreadId(0) {}
    ReplicationCoordinatorExternalStateImpl::~ReplicationCoordinatorExternalStateImpl() {}

    void ReplicationCoordinatorExternalStateImpl::startThreads() {
        boost::lock_guard<boost::mutex> lk(_threadMutex);
        if (_startedThreads) {
            return;
        }
        log() << "Starting replication applier threads";
        _applierThread.reset(new boost::thread(runSyncThread));
        BackgroundSync* bgsync = BackgroundSync::get();
        _producerThread.reset(new boost::thread(stdx::bind(&BackgroundSync::producerThread,
                                                           bgsync)));
        _syncSourceFeedbackThread.reset(new boost::thread(stdx::bind(&SyncSourceFeedback::run,
                                                                     &_syncSourceFeedback)));
        _startedThreads = true;
    }

    void ReplicationCoordinatorExternalStateImpl::startMasterSlave(OperationContext* txn) {
        repl::startMasterSlave(txn);
    }

    void ReplicationCoordinatorExternalStateImpl::shutdown() {
        boost::lock_guard<boost::mutex> lk(_threadMutex);
        if (_startedThreads) {
            log() << "Stopping replication applier threads";
            _syncSourceFeedback.shutdown();
            _syncSourceFeedbackThread->join();
            _applierThread->join();
            BackgroundSync* bgsync = BackgroundSync::get();
            bgsync->shutdown();
            _producerThread->join();
        }
    }

    void ReplicationCoordinatorExternalStateImpl::initiateOplog(OperationContext* txn) {
        createOplog(txn);

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ScopedTransaction scopedXact(txn, MODE_X);
            Lock::GlobalWrite globalWrite(txn->lockState());

            WriteUnitOfWork wuow(txn);
            getGlobalServiceContext()->getOpObserver()->onOpMessage(txn, BSON("msg" << "initiating set"));
            wuow.commit();
        } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "initiate oplog entry", "local.oplog.rs");
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
            if (!Helpers::getSingleton(txn, meCollectionName, me) ||
                    !me.hasField("host") ||
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
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn,
                                                  "load replica set config",
                                                  configCollectionName);
        }
        catch (const DBException& ex) {
            return StatusWith<BSONObj>(ex.toStatus());
        }
    }

    Status ReplicationCoordinatorExternalStateImpl::storeLocalConfigDocument(
            OperationContext* txn,
            const BSONObj& config) {
        try {
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock dbWriteLock(txn->lockState(), configDatabaseName, MODE_X);
                Helpers::putSingleton(txn, configCollectionName, config);
                return Status::OK();
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn,
                                                  "save replica set config",
                                                  configCollectionName);
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }

    }

    StatusWith<LastVote> ReplicationCoordinatorExternalStateImpl::loadLocalLastVoteDocument(
            OperationContext* txn) {
        try {
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                BSONObj lastVoteObj;
                if (!Helpers::getSingleton(txn, lastVoteCollectionName, lastVoteObj)) {
                    return StatusWith<LastVote>(
                            ErrorCodes::NoMatchingDocument,
                            str::stream() << "Did not find replica set lastVote document in "
                                          << lastVoteCollectionName);
                }
                LastVote lastVote;
                lastVote.initialize(lastVoteObj);
                return StatusWith<LastVote>(lastVote);
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn,
                                                  "load replica set lastVote",
                                                  lastVoteCollectionName);
        }
        catch (const DBException& ex) {
            return StatusWith<LastVote>(ex.toStatus());
        }
    }

    Status ReplicationCoordinatorExternalStateImpl::storeLocalLastVoteDocument(
            OperationContext* txn,
            const LastVote& lastVote) {
        BSONObj lastVoteObj = lastVote.toBSON();
        try {
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock dbWriteLock(txn->lockState(), lastVoteDatabaseName, MODE_X);
                Helpers::putSingleton(txn, lastVoteCollectionName, lastVoteObj);
                return Status::OK();
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn,
                                                  "save replica set lastVote",
                                                  lastVoteCollectionName);
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }

    }

    void ReplicationCoordinatorExternalStateImpl::setGlobalTimestamp(const Timestamp& newTime) {
        setNewOptime(newTime);
    }

    StatusWith<OpTime> ReplicationCoordinatorExternalStateImpl::loadLastOpTime(
            OperationContext* txn) {

        // TODO: handle WriteConflictExceptions below
        try {
            BSONObj oplogEntry;
            if (!Helpers::getLast(txn, rsOplogName.c_str(), oplogEntry)) {
                return StatusWith<OpTime>(
                        ErrorCodes::NoMatchingDocument,
                        str::stream() << "Did not find any entries in " << rsOplogName);
            }
            BSONElement tsElement = oplogEntry[tsFieldName];
            if (tsElement.eoo()) {
                return StatusWith<OpTime>(
                        ErrorCodes::NoSuchKey,
                        str::stream() << "Most recent entry in " << rsOplogName << " missing \"" <<
                        tsFieldName << "\" field");
            }
            if (tsElement.type() != bsonTimestamp) {
                return StatusWith<OpTime>(
                        ErrorCodes::TypeMismatch,
                        str::stream() << "Expected type of \"" << tsFieldName <<
                        "\" in most recent " << rsOplogName <<
                        " entry to have type Timestamp, but found " << typeName(tsElement.type()));
            }
            // TODO(siyuan) add term
            return StatusWith<OpTime>(OpTime(tsElement.timestamp(), 0));
        }
        catch (const DBException& ex) {
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
        MessagingPort::closeAllSockets(
            ReplicationExecutor::NetworkInterface::kMessagingPortKeepOpen);
    }

    void ReplicationCoordinatorExternalStateImpl::killAllUserOperations(OperationContext* txn) {
        ServiceContext* environment = getGlobalServiceContext();
        environment->killAllUserOperations(txn);
    }

    void ReplicationCoordinatorExternalStateImpl::clearShardingState() {
        shardingState.clearCollectionMetadata();
    }

    void ReplicationCoordinatorExternalStateImpl::signalApplierToChooseNewSyncSource() {
        BackgroundSync::get()->clearSyncTarget();
    }

    OperationContext* ReplicationCoordinatorExternalStateImpl::createOperationContext(
            const std::string& threadName) {
        Client::initThreadIfNotAlready(threadName.c_str());
        return new OperationContextImpl;
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

} // namespace repl
} // namespace mongo
