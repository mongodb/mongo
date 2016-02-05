// @file oplog.cpp

/**
*    Copyright (C) 2008-2014 MongoDB Inc.
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

#include "mongo/db/repl/oplog.h"

#include <deque>
#include <set>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/apply_ops.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/snapshot_thread.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/random.h"
#include "mongo/s/d_state.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

namespace repl {
std::string rsOplogName = "local.oplog.rs";
std::string masterSlaveOplogName = "local.oplog.$main";
int OPLOG_VERSION = 2;

MONGO_FP_DECLARE(disableSnapshotting);

namespace {
// cached copies of these...so don't rename them, drop them, etc.!!!
Database* _localDB = nullptr;
Collection* _localOplogCollection = nullptr;

PseudoRandom hashGenerator(std::unique_ptr<SecureRandom>(SecureRandom::create())->nextInt64());

// Synchronizes the section where a new Timestamp is generated and when it actually
// appears in the oplog.
stdx::mutex newOpMutex;
stdx::condition_variable newTimestampNotifier;

static std::string _oplogCollectionName;

// so we can fail the same way
void checkOplogInsert(Status result) {
    massert(17322, str::stream() << "write to oplog failed: " << result.toString(), result.isOK());
}

struct OplogSlot {
    OpTime opTime;
    int64_t hash;
};

/**
 * Allocates an optime for a new entry in the oplog, and updates the replication coordinator to
 * reflect that new optime.  Returns the new optime and the correct value of the "h" field for
 * the new oplog entry.
 *
 * NOTE: From the time this function returns to the time that the new oplog entry is written
 * to the storage system, all errors must be considered fatal.  This is because the this
 * function registers the new optime with the storage system and the replication coordinator,
 * and provides no facility to revert those registrations on rollback.
 */
OplogSlot getNextOpTime(OperationContext* txn,
                        Collection* oplog,
                        ReplicationCoordinator* replCoord,
                        ReplicationCoordinator::Mode replicationMode) {
    synchronizeOnCappedInFlightResource(txn->lockState(), oplog->ns());

    OplogSlot slot;
    slot.hash = 0;
    long long term = OpTime::kUninitializedTerm;

    // Fetch term out of the newOpMutex.
    if (replicationMode == ReplicationCoordinator::modeReplSet &&
        replCoord->isV1ElectionProtocol()) {
        // Current term. If we're not a replset of pv=1, it remains kOldProtocolVersionTerm.
        term = replCoord->getTerm();
    }

    stdx::lock_guard<stdx::mutex> lk(newOpMutex);
    Timestamp ts = getNextGlobalTimestamp();
    newTimestampNotifier.notify_all();

    fassert(28560, oplog->getRecordStore()->oplogDiskLocRegister(txn, ts));

    // Set hash if we're in replset mode, otherwise it remains 0 in master/slave.
    if (replicationMode == ReplicationCoordinator::modeReplSet) {
        slot.hash = hashGenerator.nextInt64();
    }

    slot.opTime = OpTime(ts, term);
    return slot;
}

/**
 * This allows us to stream the oplog entry directly into data region
 * main goal is to avoid copying the o portion
 * which can be very large
 * TODO: can have this build the entire doc
 */
class OplogDocWriter : public DocWriter {
public:
    OplogDocWriter(const BSONObj& frame, const BSONObj& oField) : _frame(frame), _oField(oField) {}

    void writeDocument(char* start) const {
        char* buf = start;

        memcpy(buf, _frame.objdata(), _frame.objsize() - 1);  // don't copy final EOO

        DataView(buf).write<LittleEndian<int>>(documentSize());

        buf += (_frame.objsize() - 1);
        buf[0] = (char)Object;
        buf[1] = 'o';
        buf[2] = 0;
        memcpy(buf + 3, _oField.objdata(), _oField.objsize());
        buf += 3 + _oField.objsize();
        buf[0] = EOO;

        verify(static_cast<size_t>((buf + 1) - start) == documentSize());  // DEV?
    }

    size_t documentSize() const {
        return _frame.objsize() + _oField.objsize() + 1 /* type */ + 2 /* "o" */;
    }

private:
    BSONObj _frame;
    BSONObj _oField;
};

class UpdateReplOpTimeChange : public RecoveryUnit::Change {
public:
    UpdateReplOpTimeChange(OpTime newOpTime, ReplicationCoordinator* replCoord)
        : _newOpTime(newOpTime), _replCoord(replCoord) {}

    virtual void commit() {
        _replCoord->setMyLastAppliedOpTimeForward(_newOpTime);
    }

    virtual void rollback() {}

private:
    const OpTime _newOpTime;
    ReplicationCoordinator* _replCoord;
};

}  // namespace

void setOplogCollectionName() {
    if (getGlobalReplicationCoordinator()->getReplicationMode() ==
        ReplicationCoordinator::modeReplSet) {
        _oplogCollectionName = rsOplogName;
    } else {
        _oplogCollectionName = masterSlaveOplogName;
    }
}

namespace {

Collection* getLocalOplogCollection(OperationContext* txn, const std::string& oplogCollectionName) {
    if (_localOplogCollection)
        return _localOplogCollection;
    Lock::DBLock lk(txn->lockState(), "local", MODE_IX);
    Lock::CollectionLock lk2(txn->lockState(), oplogCollectionName, MODE_IX);

    OldClientContext ctx(txn, oplogCollectionName);
    _localDB = ctx.db();
    invariant(_localDB);
    _localOplogCollection = _localDB->getCollection(oplogCollectionName);
    massert(13347,
            "the oplog collection " + oplogCollectionName +
                " missing. did you drop it? if so, restart the server",
            _localOplogCollection);
    return _localOplogCollection;
}

bool oplogDisabled(OperationContext* txn,
                   ReplicationCoordinator::Mode replicationMode,
                   const NamespaceString& nss) {
    if (replicationMode == ReplicationCoordinator::modeNone)
        return true;

    if (nss.db() == "local")
        return true;

    if (nss.isSystemDotProfile())
        return true;

    if (!txn->writesAreReplicated())
        return true;

    fassert(28626, txn->recoveryUnit());

    return false;
}

unique_ptr<OplogDocWriter> _logOpWriter(OperationContext* txn,
                                        const char* opstr,
                                        const NamespaceString& nss,
                                        const BSONObj& obj,
                                        const BSONObj* o2,
                                        bool fromMigrate,
                                        OpTime optime,
                                        long long hashNew) {
    BSONObjBuilder b(256);

    b.append("ts", optime.getTimestamp());
    if (optime.getTerm() != -1)
        b.append("t", optime.getTerm());
    b.append("h", hashNew);
    b.append("v", OPLOG_VERSION);
    b.append("op", opstr);
    b.append("ns", nss.ns());
    if (fromMigrate)
        b.appendBool("fromMigrate", true);
    if (o2)
        b.append("o2", *o2);

    return stdx::make_unique<OplogDocWriter>(OplogDocWriter(b.obj(), obj));
}
}  // end anon namespace

// Truncates the oplog to but excluding the "truncateTimestamp" entry.
void truncateOplogTo(OperationContext* txn, Timestamp truncateTimestamp) {
    const NamespaceString oplogNss(rsOplogName);
    ScopedTransaction transaction(txn, MODE_IX);
    AutoGetDb autoDb(txn, oplogNss.db(), MODE_IX);
    Lock::CollectionLock oplogCollectionLoc(txn->lockState(), oplogNss.ns(), MODE_X);
    Collection* oplogCollection = autoDb.getDb()->getCollection(oplogNss);
    if (!oplogCollection) {
        fassertFailedWithStatusNoTrace(
            28820,
            Status(ErrorCodes::NamespaceNotFound, str::stream() << "Can't find " << rsOplogName));
    }

    // Scan through oplog in reverse, from latest entry to first, to find the truncateTimestamp.
    bool foundSomethingToTruncate = false;
    RecordId lastRecordId;
    BSONObj lastOplogEntry;
    auto oplogRs = oplogCollection->getRecordStore();
    auto oplogReverseCursor = oplogRs->getCursor(txn, false);
    bool first = true;
    while (auto next = oplogReverseCursor->next()) {
        lastOplogEntry = next->data.releaseToBson();
        lastRecordId = next->id;

        const auto tsElem = lastOplogEntry["ts"];

        if (first) {
            if (tsElem.eoo())
                LOG(2) << "Oplog tail entry: " << lastOplogEntry;
            else
                LOG(2) << "Oplog tail entry ts field: " << tsElem;
            first = false;
        }

        if (tsElem.timestamp() == truncateTimestamp) {
            break;
        } else if (tsElem.timestamp() < truncateTimestamp) {
            fassertFailedWithStatusNoTrace(34411,
                                           Status(ErrorCodes::OplogOutOfOrder,
                                                  str::stream() << "Can't find "
                                                                << truncateTimestamp.toString()
                                                                << " to truncate from!"));
        }

        foundSomethingToTruncate = true;
    }

    if (foundSomethingToTruncate) {
        oplogCollection->temp_cappedTruncateAfter(txn, lastRecordId, false);
    }
}

/* we write to local.oplog.rs:
     { ts : ..., h: ..., v: ..., op: ..., etc }
   ts: an OpTime timestamp
   h: hash
   v: version
   op:
    "i" insert
    "u" update
    "d" delete
    "c" db cmd
    "db" declares presence of a database (ns is set to the db name + '.')
    "n" no op

   bb param:
     if not null, specifies a boolean to pass along to the other side as b: param.
     used for "justOne" or "upsert" flags on 'd', 'u'
*/
void _logOpsInner(OperationContext* txn,
                  const char* opstr,
                  const NamespaceString& nss,
                  const vector<unique_ptr<OplogDocWriter>>& writers,
                  bool fromMigrate,
                  Collection* oplogCollection,
                  ReplicationCoordinator::Mode replicationMode,
                  bool updateReplOpTime,
                  OpTime finalOpTime) {
    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();

    if (nss.size() && replicationMode == ReplicationCoordinator::modeReplSet &&
        !replCoord->canAcceptWritesFor(nss)) {
        severe() << "logOp() but can't accept write to collection " << nss.ns();
        fassertFailed(17405);
    }

    // we jump through a bunch of hoops here to avoid copying the obj buffer twice --
    // instead we do a single copy to the destination in the record store.
    for (auto it = writers.begin(); it != writers.end(); it++)
        checkOplogInsert(oplogCollection->insertDocument(txn, it->get(), false));

    // Set replCoord last optime only after we're sure the WUOW didn't abort and roll back.
    if (updateReplOpTime)
        txn->recoveryUnit()->registerChange(new UpdateReplOpTimeChange(finalOpTime, replCoord));

    ReplClientInfo::forClient(txn->getClient()).setLastOp(finalOpTime);
}

void _logOp(OperationContext* txn,
            const char* opstr,
            const char* ns,
            const BSONObj& obj,
            const BSONObj* o2,
            bool fromMigrate,
            const std::string& oplogName,
            ReplicationCoordinator::Mode replMode,
            bool updateOpTime) {
    NamespaceString nss(ns);
    if (oplogDisabled(txn, replMode, nss))
        return;

    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
    vector<unique_ptr<OplogDocWriter>> writers;
    Collection* oplog = getLocalOplogCollection(txn, oplogName);
    Lock::DBLock lk(txn->lockState(), "local", MODE_IX);
    Lock::CollectionLock lock(txn->lockState(), oplogName, MODE_IX);
    auto slot = getNextOpTime(txn, oplog, replCoord, replMode);
    auto writer = _logOpWriter(txn, opstr, nss, obj, o2, fromMigrate, slot.opTime, slot.hash);
    writers.emplace_back(std::move(writer));
    _logOpsInner(txn, opstr, nss, writers, fromMigrate, oplog, replMode, updateOpTime, slot.opTime);
}

void logOps(OperationContext* txn,
            const char* opstr,
            const NamespaceString& nss,
            std::vector<BSONObj>::const_iterator begin,
            std::vector<BSONObj>::const_iterator end,
            bool fromMigrate) {
    ReplicationCoordinator::Mode replMode = ReplicationCoordinator::get(txn)->getReplicationMode();

    invariant(begin != end);
    if (oplogDisabled(txn, replMode, nss))
        return;

    vector<unique_ptr<OplogDocWriter>> writers;
    writers.reserve(end - begin);
    OpTime finalOpTime;
    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
    Collection* oplog = getLocalOplogCollection(txn, _oplogCollectionName);
    Lock::DBLock lk(txn->lockState(), "local", MODE_IX);
    Lock::CollectionLock lock(txn->lockState(), _oplogCollectionName, MODE_IX);
    for (auto it = begin; it != end; it++) {
        auto slot = getNextOpTime(txn, oplog, replCoord, replMode);
        finalOpTime = slot.opTime;
        auto writer = _logOpWriter(txn, opstr, nss, *it, NULL, fromMigrate, slot.opTime, slot.hash);
        writers.emplace_back(std::move(writer));
    }
    _logOpsInner(txn, opstr, nss, writers, fromMigrate, oplog, replMode, true, finalOpTime);
}


void logOp(OperationContext* txn,
           const char* opstr,
           const char* ns,
           const BSONObj& obj,
           const BSONObj* o2,
           bool fromMigrate) {
    ReplicationCoordinator::Mode replMode = ReplicationCoordinator::get(txn)->getReplicationMode();
    _logOp(txn, opstr, ns, obj, o2, fromMigrate, _oplogCollectionName, replMode, true);
}

OpTime writeOpsToOplog(OperationContext* txn, const std::vector<BSONObj>& ops) {
    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();

    OpTime lastOptime;
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        lastOptime = replCoord->getMyLastAppliedOpTime();
        invariant(!ops.empty());
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock lk(txn->lockState(), "local", MODE_X);

        if (_localOplogCollection == 0) {
            OldClientContext ctx(txn, rsOplogName);

            _localDB = ctx.db();
            verify(_localDB);
            _localOplogCollection = _localDB->getCollection(rsOplogName);
            massert(13389,
                    "local.oplog.rs missing. did you drop it? if so restart server",
                    _localOplogCollection);
        }

        OldClientContext ctx(txn, rsOplogName, _localDB);
        WriteUnitOfWork wunit(txn);

        checkOplogInsert(
            _localOplogCollection->insertDocuments(txn, ops.begin(), ops.end(), false));
        lastOptime =
            fassertStatusOK(ErrorCodes::InvalidBSON, OpTime::parseFromOplogEntry(ops.back()));
        wunit.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "writeOps", _localOplogCollection->ns().ns());

    return lastOptime;
}

void createOplog(OperationContext* txn, const std::string& oplogCollectionName, bool replEnabled) {
    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite lk(txn->lockState());

    const ReplSettings& replSettings = ReplicationCoordinator::get(txn)->getSettings();

    OldClientContext ctx(txn, oplogCollectionName);
    Collection* collection = ctx.db()->getCollection(oplogCollectionName);

    if (collection) {
        if (replSettings.getOplogSizeBytes() != 0) {
            const CollectionOptions oplogOpts =
                collection->getCatalogEntry()->getCollectionOptions(txn);

            int o = (int)(oplogOpts.cappedSize / (1024 * 1024));
            int n = (int)(replSettings.getOplogSizeBytes() / (1024 * 1024));
            if (n != o) {
                stringstream ss;
                ss << "cmdline oplogsize (" << n << ") different than existing (" << o
                   << ") see: http://dochub.mongodb.org/core/increase-oplog";
                log() << ss.str() << endl;
                throw UserException(13257, ss.str());
            }
        }

        if (!replEnabled)
            initTimestampFromOplog(txn, oplogCollectionName);
        return;
    }

    /* create an oplog collection, if it doesn't yet exist. */
    long long sz = 0;
    if (replSettings.getOplogSizeBytes() != 0) {
        sz = replSettings.getOplogSizeBytes();
    } else {
        /* not specified. pick a default size */
        sz = 50LL * 1024LL * 1024LL;
        if (sizeof(int*) >= 8) {
#if defined(__APPLE__)
            // typically these are desktops (dev machines), so keep it smallish
            sz = (256 - 64) * 1024 * 1024;
#else
            sz = 990LL * 1024 * 1024;
            double free = File::freeSpace(storageGlobalParams.dbpath);  //-1 if call not supported.
            long long fivePct = static_cast<long long>(free * 0.05);
            if (fivePct > sz)
                sz = fivePct;
            // we use 5% of free space up to 50GB (1TB free)
            static long long upperBound = 50LL * 1024 * 1024 * 1024;
            if (fivePct > upperBound)
                sz = upperBound;
#endif
        }
    }

    log() << "******" << endl;
    log() << "creating replication oplog of size: " << (int)(sz / (1024 * 1024)) << "MB..." << endl;

    CollectionOptions options;
    options.capped = true;
    options.cappedSize = sz;
    options.autoIndexId = CollectionOptions::NO;

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        WriteUnitOfWork uow(txn);
        invariant(ctx.db()->createCollection(txn, oplogCollectionName, options));
        if (!replEnabled)
            getGlobalServiceContext()->getOpObserver()->onOpMessage(txn, BSONObj());
        uow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", oplogCollectionName);

    /* sync here so we don't get any surprising lag later when we try to sync */
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
    storageEngine->flushAllFiles(true);
    log() << "******" << endl;
}

void createOplog(OperationContext* txn) {
    const auto replEnabled = ReplicationCoordinator::get(txn)->getReplicationMode() ==
        ReplicationCoordinator::modeReplSet;
    createOplog(txn, _oplogCollectionName, replEnabled);
}

// -------------------------------------

namespace {
NamespaceString parseNs(const string& ns, const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    uassert(28635,
            "no collection name specified",
            first.canonicalType() == canonicalizeBSONType(mongo::String) &&
                first.valuestrsize() > 0);
    std::string coll = first.valuestr();
    return NamespaceString(NamespaceString(ns).db().toString(), coll);
}

using OpApplyFn = stdx::function<Status(OperationContext*, const char*, BSONObj&)>;

struct ApplyOpMetadata {
    OpApplyFn applyFunc;
    std::set<ErrorCodes::Error> acceptableErrors;

    ApplyOpMetadata(OpApplyFn fun) {
        applyFunc = fun;
    }

    ApplyOpMetadata(OpApplyFn fun, std::set<ErrorCodes::Error> theAcceptableErrors) {
        applyFunc = fun;
        acceptableErrors = theAcceptableErrors;
    }
};

std::map<std::string, ApplyOpMetadata> opsMap = {
    {"create",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd)
          -> Status { return createCollection(txn, NamespaceString(ns).db().toString(), cmd); },
      {ErrorCodes::NamespaceExists}}},
    {"collMod",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd) -> Status {
         BSONObjBuilder resultWeDontCareAbout;
         return collMod(txn, parseNs(ns, cmd), cmd, &resultWeDontCareAbout);
     }}},
    {"dropDatabase",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd)
          -> Status { return dropDatabase(txn, NamespaceString(ns).db().toString()); },
      {ErrorCodes::NamespaceNotFound}}},
    {"drop",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd) -> Status {
         BSONObjBuilder resultWeDontCareAbout;
         return dropCollection(txn, parseNs(ns, cmd), resultWeDontCareAbout);
     },
      // IllegalOperation is necessary because in 3.0 we replicate drops of system.profile
      // TODO(dannenberg) remove IllegalOperation once we no longer need 3.0 compatibility
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IllegalOperation}}},
    // deleteIndex(es) is deprecated but still works as of April 10, 2015
    {"deleteIndex",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd) -> Status {
         BSONObjBuilder resultWeDontCareAbout;
         return dropIndexes(txn, parseNs(ns, cmd), cmd, &resultWeDontCareAbout);
     },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"deleteIndexes",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd) -> Status {
         BSONObjBuilder resultWeDontCareAbout;
         return dropIndexes(txn, parseNs(ns, cmd), cmd, &resultWeDontCareAbout);
     },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"dropIndex",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd) -> Status {
         BSONObjBuilder resultWeDontCareAbout;
         return dropIndexes(txn, parseNs(ns, cmd), cmd, &resultWeDontCareAbout);
     },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"dropIndexes",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd) -> Status {
         BSONObjBuilder resultWeDontCareAbout;
         return dropIndexes(txn, parseNs(ns, cmd), cmd, &resultWeDontCareAbout);
     },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"renameCollection",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd) -> Status {
         return renameCollection(txn,
                                 NamespaceString(cmd.firstElement().valuestrsafe()),
                                 NamespaceString(cmd["to"].valuestrsafe()),
                                 cmd["stayTemp"].trueValue(),
                                 cmd["dropTarget"].trueValue());
     },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::NamespaceExists}}},
    {"applyOps",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd) -> Status {
         BSONObjBuilder resultWeDontCareAbout;
         return applyOps(txn, nsToDatabase(ns), cmd, &resultWeDontCareAbout);
     },
      {ErrorCodes::UnknownError}}},
    {"convertToCapped",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd)
          -> Status { return convertToCapped(txn, parseNs(ns, cmd), cmd["size"].number()); }}},
    {"emptycapped",
     {[](OperationContext* txn, const char* ns, BSONObj& cmd)
          -> Status { return emptyCapped(txn, parseNs(ns, cmd)); }}},
};

}  // namespace

// @return failure status if an update should have happened and the document DNE.
// See replset initial sync code.
Status applyOperation_inlock(OperationContext* txn,
                             Database* db,
                             const BSONObj& op,
                             bool convertUpdateToUpsert,
                             IncrementOpsAppliedStatsFn incrementOpsAppliedStats) {
    LOG(3) << "applying op: " << op;

    OpCounters* opCounters = txn->writesAreReplicated() ? &globalOpCounters : &replOpCounters;

    const char* names[] = {"o", "ns", "op", "b", "o2"};
    BSONElement fields[5];
    op.getFields(5, names, fields);
    BSONElement& fieldO = fields[0];
    BSONElement& fieldNs = fields[1];
    BSONElement& fieldOp = fields[2];
    BSONElement& fieldB = fields[3];
    BSONElement& fieldO2 = fields[4];

    BSONObj o;
    if (fieldO.isABSONObj())
        o = fieldO.embeddedObject();

    const StringData ns = fieldNs.valueStringData();

    BSONObj o2;
    if (fieldO2.isABSONObj())
        o2 = fieldO2.Obj();

    bool valueB = fieldB.booleanSafe();

    if (nsIsFull(ns)) {
        if (supportsDocLocking()) {
            // WiredTiger, and others requires MODE_IX since the applier threads driving
            // this allow writes to the same collection on any thread.
            dassert(txn->lockState()->isCollectionLockedForMode(ns, MODE_IX));
        } else {
            // mmapV1 ensures that all operations to the same collection are executed from
            // the same worker thread, so it takes an exclusive lock (MODE_X)
            dassert(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
        }
    }
    Collection* collection = db->getCollection(ns);
    IndexCatalog* indexCatalog = collection == nullptr ? nullptr : collection->getIndexCatalog();

    // operation type -- see logOp() comments for types
    const char* opType = fieldOp.valuestrsafe();
    invariant(*opType != 'c');  // commands are processed in applyCommand_inlock()

    if (*opType == 'i') {
        if (nsToCollectionSubstring(ns) == "system.indexes") {
            uassert(ErrorCodes::NoSuchKey,
                    str::stream() << "Missing expected index spec in field 'o': " << op,
                    !fieldO.eoo());
            uassert(ErrorCodes::TypeMismatch,
                    str::stream() << "Expected object for index spec in field 'o': " << op,
                    fieldO.isABSONObj());

            std::string indexNs;
            uassertStatusOK(bsonExtractStringField(o, "ns", &indexNs));
            const NamespaceString indexNss(indexNs);
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid namespace in index spec: " << op,
                    indexNss.isValid());
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Database name mismatch for database ("
                                  << nsToDatabaseSubstring(ns) << ") while creating index: " << op,
                    nsToDatabaseSubstring(ns) == indexNss.db());

            opCounters->gotInsert();
            if (o["background"].trueValue()) {
                Lock::TempRelease release(txn->lockState());
                if (txn->lockState()->isLocked()) {
                    // If TempRelease fails, background index build will deadlock.
                    LOG(3) << "apply op: building background index " << o
                           << " in the foreground because temp release failed";
                    IndexBuilder builder(o);
                    Status status = builder.buildInForeground(txn, db);
                    uassertStatusOK(status);
                } else {
                    IndexBuilder* builder = new IndexBuilder(o);
                    // This spawns a new thread and returns immediately.
                    builder->go();
                    // Wait for thread to start and register itself
                    IndexBuilder::waitForBgIndexStarting();
                }
            } else {
                IndexBuilder builder(o);
                Status status = builder.buildInForeground(txn, db);
                uassertStatusOK(status);
            }
            // Since this is an index operation we can return without falling through.
            if (incrementOpsAppliedStats) {
                incrementOpsAppliedStats();
            }
            return Status::OK();
        }
        uassert(
            ErrorCodes::NamespaceNotFound,
            str::stream() << "Failed to apply insert due to missing collection: " << op.toString(),
            collection);

        if (fieldO.type() == Array) {
            // Batched inserts.
            Status status{ErrorCodes::NotYetInitialized, ""};

            std::vector<BSONObj> insertObjs;
            for (auto elem : fieldO.Array()) {
                insertObjs.push_back(elem.Obj());
            }

            WriteUnitOfWork wuow(txn);
            status = collection->insertDocuments(txn, insertObjs.begin(), insertObjs.end(), true);
            if (!status.isOK()) {
                return status;
            }
            wuow.commit();
            for (auto entry : insertObjs) {
                opCounters->gotInsert();
                if (incrementOpsAppliedStats) {
                    incrementOpsAppliedStats();
                }
            }
        } else {
            // Single insert.
            opCounters->gotInsert();

            // No _id.
            // This indicates an issue with the upstream server:
            //     The oplog entry is corrupted; or
            //     The version of the upstream server is obsolete.
            uassert(ErrorCodes::NoSuchKey,
                    str::stream() << "Failed to apply insert due to missing _id: " << op.toString(),
                    o.hasField("_id"));

            // 1. Try insert first
            // 2. If okay, commit
            // 3. If not, do update (and commit)
            // 4. If both !Ok, return status
            Status status{ErrorCodes::NotYetInitialized, ""};
            {
                WriteUnitOfWork wuow(txn);
                try {
                    status = collection->insertDocument(txn, o, true);
                } catch (DBException dbe) {
                    status = dbe.toStatus();
                }
                if (status.isOK()) {
                    wuow.commit();
                }
            }
            // Now see if we need to do an update, based on duplicate _id index key
            if (!status.isOK()) {
                if (status.code() != ErrorCodes::DuplicateKey) {
                    return status;
                }

                // Do update on DuplicateKey errors.
                // This will only be on the _id field in replication,
                // since we disable non-_id unique constraint violations.
                OpDebug debug;
                BSONObjBuilder b;
                b.append(o.getField("_id"));

                const NamespaceString requestNs(ns);
                UpdateRequest request(requestNs);

                request.setQuery(b.done());
                request.setUpdates(o);
                request.setUpsert();
                UpdateLifecycleImpl updateLifecycle(true, requestNs);
                request.setLifecycle(&updateLifecycle);

                UpdateResult res = update(txn, db, request, &debug);
                if (res.numMatched == 0 && res.upserted.isEmpty()) {
                    error() << "No document was updated even though we got a DuplicateKey "
                               "error when inserting";
                    fassertFailedNoTrace(28750);
                }
            }
            if (incrementOpsAppliedStats) {
                incrementOpsAppliedStats();
            }
        }
    } else if (*opType == 'u') {
        opCounters->gotUpdate();

        OpDebug debug;
        BSONObj updateCriteria = o2;
        const bool upsert = valueB || convertUpdateToUpsert;

        uassert(ErrorCodes::NoSuchKey,
                str::stream() << "Failed to apply update due to missing _id: " << op.toString(),
                updateCriteria.hasField("_id"));

        const NamespaceString requestNs(ns);
        UpdateRequest request(requestNs);

        request.setQuery(updateCriteria);
        request.setUpdates(o);
        request.setUpsert(upsert);
        UpdateLifecycleImpl updateLifecycle(true, requestNs);
        request.setLifecycle(&updateLifecycle);

        UpdateResult ur = update(txn, db, request, &debug);

        if (ur.numMatched == 0 && ur.upserted.isEmpty()) {
            if (ur.modifiers) {
                if (updateCriteria.nFields() == 1) {
                    // was a simple { _id : ... } update criteria
                    string msg = str::stream() << "failed to apply update: " << op.toString();
                    error() << msg;
                    return Status(ErrorCodes::OperationFailed, msg);
                }
                // Need to check to see if it isn't present so we can exit early with a
                // failure. Note that adds some overhead for this extra check in some cases,
                // such as an updateCriteria
                // of the form
                //   { _id:..., { x : {$size:...} }
                // thus this is not ideal.
                if (collection == NULL ||
                    (indexCatalog->haveIdIndex(txn) &&
                     Helpers::findById(txn, collection, updateCriteria).isNull()) ||
                    // capped collections won't have an _id index
                    (!indexCatalog->haveIdIndex(txn) &&
                     Helpers::findOne(txn, collection, updateCriteria, false).isNull())) {
                    string msg = str::stream() << "couldn't find doc: " << op.toString();
                    error() << msg;
                    return Status(ErrorCodes::OperationFailed, msg);
                }

                // Otherwise, it's present; zero objects were updated because of additional
                // specifiers in the query for idempotence
            } else {
                // this could happen benignly on an oplog duplicate replay of an upsert
                // (because we are idempotent),
                // if an regular non-mod update fails the item is (presumably) missing.
                if (!upsert) {
                    string msg = str::stream() << "update of non-mod failed: " << op.toString();
                    error() << msg;
                    return Status(ErrorCodes::OperationFailed, msg);
                }
            }
        }
        if (incrementOpsAppliedStats) {
            incrementOpsAppliedStats();
        }
    } else if (*opType == 'd') {
        opCounters->gotDelete();

        uassert(ErrorCodes::NoSuchKey,
                str::stream() << "Failed to apply delete due to missing _id: " << op.toString(),
                o.hasField("_id"));

        if (opType[1] == 0) {
            deleteObjects(txn, collection, ns, o, PlanExecutor::YIELD_MANUAL, /*justOne*/ valueB);
        } else
            verify(opType[1] == 'b');  // "db" advertisement
        if (incrementOpsAppliedStats) {
            incrementOpsAppliedStats();
        }
    } else if (*opType == 'n') {
        // no op
        if (incrementOpsAppliedStats) {
            incrementOpsAppliedStats();
        }
    } else {
        throw MsgAssertionException(
            14825, str::stream() << "error in applyOperation : unknown opType " << *opType);
    }

    // AuthorizationManager's logOp method registers a RecoveryUnit::Change
    // and to do so we need to have begun a UnitOfWork
    WriteUnitOfWork wuow(txn);
    getGlobalAuthorizationManager()->logOp(
        txn, opType, ns.toString().c_str(), o, fieldO2.isABSONObj() ? &o2 : NULL);
    wuow.commit();

    return Status::OK();
}

Status applyCommand_inlock(OperationContext* txn, const BSONObj& op) {
    const char* names[] = {"o", "ns", "op"};
    BSONElement fields[3];
    op.getFields(3, names, fields);
    BSONElement& fieldO = fields[0];
    BSONElement& fieldNs = fields[1];
    BSONElement& fieldOp = fields[2];

    const char* opType = fieldOp.valuestrsafe();
    invariant(*opType == 'c');  // only commands are processed here

    if (fieldO.eoo()) {
        return Status(ErrorCodes::NoSuchKey, "Missing expected field 'o'");
    }

    if (!fieldO.isABSONObj()) {
        return Status(ErrorCodes::BadValue, "Expected object for field 'o'");
    }

    BSONObj o = fieldO.embeddedObject();

    const char* ns = fieldNs.valuestrsafe();

    // Applying commands in repl is done under Global W-lock, so it is safe to not
    // perform the current DB checks after reacquiring the lock.
    invariant(txn->lockState()->isW());

    bool done = false;

    while (!done) {
        auto op = opsMap.find(o.firstElementFieldName());
        if (op == opsMap.end()) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Invalid key '" << o.firstElementFieldName()
                                                    << "' found in field 'o'");
        }
        ApplyOpMetadata curOpToApply = op->second;
        Status status = Status::OK();
        try {
            status = curOpToApply.applyFunc(txn, ns, o);
        } catch (...) {
            status = exceptionToStatus();
        }
        switch (status.code()) {
            case ErrorCodes::WriteConflict: {
                // Need to throw this up to a higher level where it will be caught and the
                // operation retried.
                throw WriteConflictException();
            }
            case ErrorCodes::BackgroundOperationInProgressForDatabase: {
                Lock::TempRelease release(txn->lockState());

                BackgroundOperation::awaitNoBgOpInProgForDb(nsToDatabaseSubstring(ns));
                txn->recoveryUnit()->abandonSnapshot();
                break;
            }
            case ErrorCodes::BackgroundOperationInProgressForNamespace: {
                Lock::TempRelease release(txn->lockState());

                Command* cmd = Command::findCommand(o.firstElement().fieldName());
                invariant(cmd);
                BackgroundOperation::awaitNoBgOpInProgForNs(cmd->parseNs(nsToDatabase(ns), o));
                txn->recoveryUnit()->abandonSnapshot();
                break;
            }
            default:
                if (_oplogCollectionName == masterSlaveOplogName) {
                    error() << "Failed command " << o << " on " << nsToDatabaseSubstring(ns)
                            << " with status " << status << " during oplog application";
                } else if (curOpToApply.acceptableErrors.find(status.code()) ==
                           curOpToApply.acceptableErrors.end()) {
                    error() << "Failed command " << o << " on " << nsToDatabaseSubstring(ns)
                            << " with status " << status << " during oplog application";
                    return status;
                }
            // fallthrough
            case ErrorCodes::OK:
                done = true;
                break;
        }
    }

    // AuthorizationManager's logOp method registers a RecoveryUnit::Change
    // and to do so we need to have begun a UnitOfWork
    WriteUnitOfWork wuow(txn);
    getGlobalAuthorizationManager()->logOp(txn, opType, ns, o, nullptr);
    wuow.commit();

    return Status::OK();
}

void setNewTimestamp(const Timestamp& newTime) {
    stdx::lock_guard<stdx::mutex> lk(newOpMutex);
    setGlobalTimestamp(newTime);
    newTimestampNotifier.notify_all();
}

void initTimestampFromOplog(OperationContext* txn, const std::string& oplogNS) {
    DBDirectClient c(txn);
    BSONObj lastOp = c.findOne(oplogNS, Query().sort(reverseNaturalObj), NULL, QueryOption_SlaveOk);

    if (!lastOp.isEmpty()) {
        LOG(1) << "replSet setting last Timestamp";
        const OpTime opTime = fassertStatusOK(28696, OpTime::parseFromOplogEntry(lastOp));
        setNewTimestamp(opTime.getTimestamp());
    }
}

void oplogCheckCloseDatabase(OperationContext* txn, Database* db) {
    invariant(txn->lockState()->isW());

    _localDB = nullptr;
    _localOplogCollection = nullptr;
}

void signalOplogWaiters() {
    if (_localOplogCollection) {
        _localOplogCollection->notifyCappedWaitersIfNeeded();
    }
}

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(replSnapshotThreadThrottleMicros, int, 1000);

SnapshotThread::SnapshotThread(SnapshotManager* manager)
    : _manager(manager), _thread([this] { run(); }) {}

bool SnapshotThread::shouldSleepMore(int numSleepsDone, size_t numUncommittedSnapshots) {
    const double kThrottleRatio = 1 / 20.0;
    const size_t kUncommittedSnapshotLimit = 1000;
    const size_t kUncommittedSnapshotRestartPoint = kUncommittedSnapshotLimit / 2;

    if (_inShutdown.load())
        return false;  // Exit the thread quickly without sleeping.

    if (numSleepsDone == 0)
        return true;  // Always sleep at least once.

    {
        // Enforce a limit on the number of snapshots.
        if (numUncommittedSnapshots >= kUncommittedSnapshotLimit)
            _hitSnapshotLimit = true;  // Don't create new snapshots.

        if (numUncommittedSnapshots < kUncommittedSnapshotRestartPoint)
            _hitSnapshotLimit = false;  // Begin creating new snapshots again.

        if (_hitSnapshotLimit)
            return true;
    }

    // Spread out snapshots in time by sleeping as we collect more uncommitted snapshots.
    const double numSleepsNeeded = numUncommittedSnapshots * kThrottleRatio;
    return numSleepsNeeded > numSleepsDone;
}

void SnapshotThread::run() {
    Client::initThread("SnapshotThread");
    auto& client = cc();
    auto serviceContext = client.getServiceContext();
    auto replCoord = ReplicationCoordinator::get(serviceContext);

    Timestamp lastTimestamp = {};
    while (true) {
        // This block logically belongs at the end of the loop, but having it at the top
        // simplifies handling of the "continue" cases. It is harmless to do these before the
        // first run of the loop.
        for (int numSleepsDone = 0;
             shouldSleepMore(numSleepsDone, replCoord->getNumUncommittedSnapshots());
             numSleepsDone++) {
            sleepmicros(replSnapshotThreadThrottleMicros);
            _manager->cleanupUnneededSnapshots();
        }

        {
            stdx::unique_lock<stdx::mutex> lock(newOpMutex);
            while (true) {
                if (_inShutdown.load())
                    return;

                if (_forcedSnapshotPending.load() || lastTimestamp != getLastSetTimestamp()) {
                    _forcedSnapshotPending.store(false);
                    lastTimestamp = getLastSetTimestamp();
                    break;
                }

                newTimestampNotifier.wait(lock);
            }
        }

        while (MONGO_FAIL_POINT(disableSnapshotting)) {
            sleepsecs(1);
            if (_inShutdown.load()) {
                return;
            }
        }

        try {
            auto txn = client.makeOperationContext();
            Lock::GlobalLock globalLock(txn->lockState(), MODE_IS, UINT_MAX);

            if (!replCoord->getMemberState().readable()) {
                // If our MemberState isn't readable, we may not be in a consistent state so don't
                // take snapshots. When we transition into a readable state from a non-readable
                // state, a snapshot is forced to ensure we don't miss the latest write. This must
                // be checked each time we acquire the global IS lock since that prevents the node
                // from transitioning to a !readable() state from a readable() one in the cases
                // where we shouldn't be creating a snapshot.
                continue;
            }

            SnapshotName name(0);  // assigned real value in block.
            {
                // Make sure there are no in-flight capped inserts while we create our snapshot.
                Lock::ResourceLock cappedInsertLockForOtherDb(
                    txn->lockState(), resourceCappedInFlightForOtherDb, MODE_X);
                Lock::ResourceLock cappedInsertLockForLocalDb(
                    txn->lockState(), resourceCappedInFlightForLocalDb, MODE_X);

                // Reserve the name immediately before we take our snapshot. This ensures that all
                // names that compare lower must be from points in time visible to this named
                // snapshot.
                name = replCoord->reserveSnapshotName(nullptr);

                // This establishes the view that we will name.
                _manager->prepareForCreateSnapshot(txn.get());
            }

            auto opTimeOfSnapshot = OpTime();
            {
                AutoGetCollectionForRead oplog(txn.get(), rsOplogName);
                invariant(oplog.getCollection());
                // Read the latest op from the oplog.
                auto cursor = oplog.getCollection()->getCursor(txn.get(), /*forward*/ false);
                auto record = cursor->next();
                if (!record)
                    continue;  // oplog is completely empty.

                const auto op = record->data.releaseToBson();
                opTimeOfSnapshot = fassertStatusOK(28780, OpTime::parseFromOplogEntry(op));
                invariant(!opTimeOfSnapshot.isNull());
            }

            _manager->createSnapshot(txn.get(), name);
            replCoord->onSnapshotCreate(opTimeOfSnapshot, name);
        } catch (const WriteConflictException& wce) {
            log() << "skipping storage snapshot pass due to write conflict";
            continue;
        }
    }
}

void SnapshotThread::shutdown() {
    invariant(_thread.joinable());
    {
        stdx::lock_guard<stdx::mutex> lock(newOpMutex);
        invariant(!_inShutdown.load());
        _inShutdown.store(true);
        newTimestampNotifier.notify_all();
    }
    _thread.join();
}

void SnapshotThread::forceSnapshot() {
    stdx::lock_guard<stdx::mutex> lock(newOpMutex);
    _forcedSnapshotPending.store(true);
    newTimestampNotifier.notify_all();
}

std::unique_ptr<SnapshotThread> SnapshotThread::start(ServiceContext* service) {
    if (auto manager = service->getGlobalStorageEngine()->getSnapshotManager()) {
        return std::unique_ptr<SnapshotThread>(new SnapshotThread(manager));
    }
    return {};
}

}  // namespace repl
}  // namespace mongo
