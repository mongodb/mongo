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
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/apply_ops.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
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
#include "mongo/db/service_context.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/snapshot_thread.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage_options.h"
#include "mongo/s/d_state.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;

namespace repl {
std::string rsOplogName = "local.oplog.rs";
std::string masterSlaveOplogName = "local.oplog.$main";
int OPLOG_VERSION = 2;

namespace {
// cached copies of these...so don't rename them, drop them, etc.!!!
Database* _localDB = nullptr;
Collection* _localOplogCollection = nullptr;

// Synchronizes the section where a new Timestamp is generated and when it actually
// appears in the oplog.
stdx::mutex newOpMutex;
stdx::condition_variable newTimestampNotifier;

static std::string _oplogCollectionName;

const std::string kTimestampFieldName = "ts";
const std::string kTermFieldName = "t";

// so we can fail the same way
void checkOplogInsert(StatusWith<RecordId> result) {
    massert(17322,
            str::stream() << "write to oplog failed: " << result.getStatus().toString(),
            result.isOK());
}

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
std::pair<OpTime, long long> getNextOpTime(OperationContext* txn,
                                           Collection* oplog,
                                           const char* ns,
                                           ReplicationCoordinator* replCoord,
                                           const char* opstr) {
    synchronizeOnCappedInFlightResource(txn->lockState());

    stdx::lock_guard<stdx::mutex> lk(newOpMutex);
    Timestamp ts = getNextGlobalTimestamp();
    newTimestampNotifier.notify_all();

    fassert(28560, oplog->getRecordStore()->oplogDiskLocRegister(txn, ts));

    long long hashNew = 0;
    long long term = OpTime::kProtocolVersionV0Term;

    // Set hash and term if we're in replset mode, otherwise they remain 0 in master/slave.
    if (replCoord->getReplicationMode() == ReplicationCoordinator::modeReplSet) {
        // Current term. If we're not a replset of pv=1, it remains kOldProtocolVersionTerm.
        if (replCoord->isV1ElectionProtocol()) {
            term = ReplClientInfo::forClient(txn->getClient()).getTerm();
        }

        hashNew = BackgroundSync::get()->getLastAppliedHash();

        // Check to make sure logOp() is legal at this point.
        if (*opstr == 'n') {
            // 'n' operations are always logged
            invariant(*ns == '\0');
            // 'n' operations do not advance the hash, since they are not rolled back
        } else {
            // Advance the hash
            hashNew = (hashNew * 131 + ts.asLL()) * 17 + replCoord->getMyId();

            BackgroundSync::get()->setLastAppliedHash(hashNew);
        }
    }

    OpTime opTime(ts, term);
    replCoord->setMyLastOptime(opTime);
    return std::pair<OpTime, long long>(opTime, hashNew);
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

    ~OplogDocWriter() {}

    void writeDocument(char* start) const {
        char* buf = start;

        memcpy(buf, _frame.objdata(), _frame.objsize() - 1);  // don't copy final EOO

        reinterpret_cast<int*>(buf)[0] = documentSize();

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

}  // namespace

void setOplogCollectionName() {
    if (getGlobalReplicationCoordinator()->getReplicationMode() ==
        ReplicationCoordinator::modeReplSet) {
        _oplogCollectionName = rsOplogName;
    } else {
        _oplogCollectionName = masterSlaveOplogName;
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

void _logOp(OperationContext* txn,
            const char* opstr,
            const char* ns,
            const BSONObj& obj,
            BSONObj* o2,
            bool fromMigrate) {
    NamespaceString nss(ns);
    if (nss.db() == "local") {
        return;
    }

    if (nss.isSystemDotProfile()) {
        return;
    }

    if (!getGlobalReplicationCoordinator()->isReplEnabled()) {
        return;
    }

    if (!txn->writesAreReplicated()) {
        return;
    }

    fassert(28626, txn->recoveryUnit());

    Lock::DBLock lk(txn->lockState(), "local", MODE_IX);

    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();

    if (ns[0] && replCoord->getReplicationMode() == ReplicationCoordinator::modeReplSet &&
        !replCoord->canAcceptWritesFor(nss)) {
        severe() << "logOp() but can't accept write to collection " << ns;
        fassertFailed(17405);
    }
    Lock::CollectionLock lk2(txn->lockState(), _oplogCollectionName, MODE_IX);


    if (_localOplogCollection == nullptr) {
        OldClientContext ctx(txn, _oplogCollectionName);
        _localDB = ctx.db();
        invariant(_localDB);
        _localOplogCollection = _localDB->getCollection(_oplogCollectionName);
        massert(13347,
                "the oplog collection " + _oplogCollectionName +
                    " missing. did you drop it? if so, restart the server",
                _localOplogCollection);
    }

    std::pair<OpTime, long long> slot =
        getNextOpTime(txn, _localOplogCollection, ns, replCoord, opstr);

    /* we jump through a bunch of hoops here to avoid copying the obj buffer twice --
       instead we do a single copy to the destination position in the memory mapped file.
    */

    BSONObjBuilder b(256);
    b.append(kTimestampFieldName, slot.first.getTimestamp());
    // Don't add term in protocol version 0.
    if (slot.first.getTerm() != OpTime::kProtocolVersionV0Term) {
        b.append(kTermFieldName, slot.first.getTerm());
    }
    b.append("h", slot.second);
    b.append("v", OPLOG_VERSION);
    b.append("op", opstr);
    b.append("ns", ns);
    if (fromMigrate) {
        b.appendBool("fromMigrate", true);
    }

    if (o2) {
        b.append("o2", *o2);
    }
    BSONObj partial = b.done();

    OplogDocWriter writer(partial, obj);
    checkOplogInsert(_localOplogCollection->insertDocument(txn, &writer, false));

    ReplClientInfo::forClient(txn->getClient()).setLastOp(slot.first);
}

OpTime writeOpsToOplog(OperationContext* txn, const std::deque<BSONObj>& ops) {
    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();

    OpTime lastOptime;
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        lastOptime = replCoord->getMyLastOptime();
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

        for (std::deque<BSONObj>::const_iterator it = ops.begin(); it != ops.end(); ++it) {
            const BSONObj& op = *it;
            const OpTime optime = extractOpTime(op);

            checkOplogInsert(_localOplogCollection->insertDocument(txn, op, false));

            // lastOptime and optime are successive in the log, so it's safe to compare them.
            if (!(lastOptime < optime)) {
                severe() << "replication oplog stream went back in time. "
                            "previous timestamp: " << lastOptime << " newest timestamp: " << optime
                         << ". Op being applied: " << op;
                fassertFailedNoTrace(18905);
            }
            lastOptime = optime;
        }
        wunit.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "writeOps", _localOplogCollection->ns().ns());

    BackgroundSync* bgsync = BackgroundSync::get();
    // Keep this up-to-date, in case we step up to primary.
    long long hash = ops.back()["h"].numberLong();
    bgsync->setLastAppliedHash(hash);

    return lastOptime;
}

void createOplog(OperationContext* txn) {
    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite lk(txn->lockState());

    const ReplSettings& replSettings = getGlobalReplicationCoordinator()->getSettings();
    bool rs = !replSettings.replSet.empty();

    OldClientContext ctx(txn, _oplogCollectionName);
    Collection* collection = ctx.db()->getCollection(_oplogCollectionName);

    if (collection) {
        if (replSettings.oplogSize != 0) {
            const CollectionOptions oplogOpts =
                collection->getCatalogEntry()->getCollectionOptions(txn);

            int o = (int)(oplogOpts.cappedSize / (1024 * 1024));
            int n = (int)(replSettings.oplogSize / (1024 * 1024));
            if (n != o) {
                stringstream ss;
                ss << "cmdline oplogsize (" << n << ") different than existing (" << o
                   << ") see: http://dochub.mongodb.org/core/increase-oplog";
                log() << ss.str() << endl;
                throw UserException(13257, ss.str());
            }
        }

        if (!rs)
            initTimestampFromOplog(txn, _oplogCollectionName);
        return;
    }

    /* create an oplog collection, if it doesn't yet exist. */
    long long sz = 0;
    if (replSettings.oplogSize != 0) {
        sz = replSettings.oplogSize;
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
        invariant(ctx.db()->createCollection(txn, _oplogCollectionName, options));
        if (!rs)
            getGlobalServiceContext()->getOpObserver()->onOpMessage(txn, BSONObj());
        uow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", _oplogCollectionName);

    /* sync here so we don't get any surprising lag later when we try to sync */
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
    storageEngine->flushAllFiles(true);
    log() << "******" << endl;
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
      {ErrorCodes::DatabaseNotFound}}},
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
                             bool convertUpdateToUpsert) {
    LOG(3) << "applying op: " << op << endl;

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

    const char* ns = fieldNs.valuestrsafe();

    BSONObj o2;
    if (fieldO2.isABSONObj())
        o2 = fieldO2.Obj();

    bool valueB = fieldB.booleanSafe();

    if (nsIsFull(ns)) {
        if (supportsDocLocking()) {
            // WiredTiger, and others requires MODE_IX since the applier threads driving
            // this allow writes to the same collection on any thread.
            invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_IX));
        } else {
            // mmapV1 ensures that all operations to the same collection are executed from
            // the same worker thread, so it takes an exclusive lock (MODE_X)
            invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));
        }
    }
    Collection* collection = db->getCollection(ns);
    IndexCatalog* indexCatalog = collection == nullptr ? nullptr : collection->getIndexCatalog();

    // operation type -- see logOp() comments for types
    const char* opType = fieldOp.valuestrsafe();
    invariant(*opType != 'c');  // commands are processed in applyCommand_inlock()

    if (*opType == 'i') {
        opCounters->gotInsert();

        const char* p = strchr(ns, '.');
        if (p && nsToCollectionSubstring(p) == "system.indexes") {
            if (o["background"].trueValue()) {
                IndexBuilder* builder = new IndexBuilder(o);
                // This spawns a new thread and returns immediately.
                builder->go();
                // Wait for thread to start and register itself
                Lock::TempRelease release(txn->lockState());
                IndexBuilder::waitForBgIndexStarting();
            } else {
                IndexBuilder builder(o);
                Status status = builder.buildInForeground(txn, db);
                uassertStatusOK(status);
            }
        } else {
            // do upserts for inserts as we might get replayed more than once
            OpDebug debug;

            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Failed to apply insert due to missing collection: "
                                  << op.toString(),
                    collection);

            // No _id.
            // This indicates an issue with the upstream server:
            //     The oplog entry is corrupted; or
            //     The version of the upstream server is obsolete.
            uassert(ErrorCodes::NoSuchKey,
                    str::stream() << "Failed to apply insert due to missing _id: " << op.toString(),
                    o.hasField("_id"));

            // TODO: It may be better to do an insert here, and then catch the duplicate
            // key exception and do update then. Very few upserts will not be inserts...
            BSONObjBuilder b;
            b.append(o.getField("_id"));

            const NamespaceString requestNs(ns);
            UpdateRequest request(requestNs);

            request.setQuery(b.done());
            request.setUpdates(o);
            request.setUpsert();
            UpdateLifecycleImpl updateLifecycle(true, requestNs);
            request.setLifecycle(&updateLifecycle);

            update(txn, db, request, &debug);
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

        if (ur.numMatched == 0) {
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
    } else if (*opType == 'd') {
        opCounters->gotDelete();

        uassert(ErrorCodes::NoSuchKey,
                str::stream() << "Failed to apply delete due to missing _id: " << op.toString(),
                o.hasField("_id"));

        if (opType[1] == 0) {
            deleteObjects(txn, db, ns, o, PlanExecutor::YIELD_MANUAL, /*justOne*/ valueB);
        } else
            verify(opType[1] == 'b');  // "db" advertisement
    } else if (*opType == 'n') {
        // no op
    } else {
        throw MsgAssertionException(14825,
                                    ErrorMsg("error in applyOperation : unknown opType ", *opType));
    }

    // AuthorizationManager's logOp method registers a RecoveryUnit::Change
    // and to do so we need to have begun a UnitOfWork
    WriteUnitOfWork wuow(txn);
    getGlobalAuthorizationManager()->logOp(txn, opType, ns, o, fieldO2.isABSONObj() ? &o2 : NULL);
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

    BSONObj o;
    if (fieldO.isABSONObj()) {
        o = fieldO.embeddedObject();
    }

    const char* ns = fieldNs.valuestrsafe();

    // Applying commands in repl is done under Global W-lock, so it is safe to not
    // perform the current DB checks after reacquiring the lock.
    invariant(txn->lockState()->isW());

    bool done = false;

    while (!done) {
        ApplyOpMetadata curOpToApply = opsMap.find(o.firstElementFieldName())->second;
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

void waitUpToOneSecondForTimestampChange(const Timestamp& referenceTime) {
    stdx::unique_lock<stdx::mutex> lk(newOpMutex);

    while (referenceTime == getLastSetTimestamp()) {
        if (stdx::cv_status::timeout == newTimestampNotifier.wait_for(lk, stdx::chrono::seconds(1)))
            return;
    }
}

void setNewTimestamp(const Timestamp& newTime) {
    stdx::lock_guard<stdx::mutex> lk(newOpMutex);
    setGlobalTimestamp(newTime);
    newTimestampNotifier.notify_all();
}

OpTime extractOpTime(const BSONObj& op) {
    const Timestamp ts = op[kTimestampFieldName].timestamp();
    long long term;
    // Default to -1 if the term is absent.
    fassert(28696,
            bsonExtractIntegerFieldWithDefault(
                op, kTermFieldName, OpTime::kProtocolVersionV0Term, &term));
    return OpTime(ts, term);
}

void initTimestampFromOplog(OperationContext* txn, const std::string& oplogNS) {
    DBDirectClient c(txn);
    BSONObj lastOp = c.findOne(oplogNS, Query().sort(reverseNaturalObj), NULL, QueryOption_SlaveOk);

    if (!lastOp.isEmpty()) {
        LOG(1) << "replSet setting last Timestamp";
        setNewTimestamp(lastOp[kTimestampFieldName].timestamp());
    }
}

void oplogCheckCloseDatabase(OperationContext* txn, Database* db) {
    invariant(txn->lockState()->isW());

    _localDB = nullptr;
    _localOplogCollection = nullptr;
}

SnapshotThread::SnapshotThread(SnapshotManager* manager, Callback&& onSnapshotCreate)
    : _manager(manager),
      _onSnapshotCreate(std::move(onSnapshotCreate)),
      _thread([this] { run(); }) {}

void SnapshotThread::run() {
    Client::initThread("SnapshotThread");
    auto& client = cc();
    auto serviceContext = client.getServiceContext();

    Timestamp lastTimestamp = {};
    while (true) {
        {
            stdx::unique_lock<stdx::mutex> lock(newOpMutex);
            while (true) {
                if (_inShutdown)
                    return;

                if (_forcedSnapshotPending || lastTimestamp != getLastSetTimestamp()) {
                    _forcedSnapshotPending = false;
                    lastTimestamp = getLastSetTimestamp();
                    break;
                }

                newTimestampNotifier.wait(lock);
            }
        }

        try {
            auto txn = client.makeOperationContext();
            Lock::GlobalLock globalLock(txn->lockState(), MODE_IS, UINT_MAX);

            if (!ReplicationCoordinator::get(serviceContext)->getMemberState().readable()) {
                // If our MemberState isn't readable, we may not be in a consistent state so don't
                // take snapshots. When we transition into a readable state from a non-readable
                // state, a snapshot is forced to ensure we don't miss the latest write. This must
                // be checked each time we acquire the global IS lock since that prevents the node
                // from transitioning to a !readable() state from a readable() one.
                continue;
            }

            {
                // Make sure there are no in-flight capped inserts while we create our snapshot.
                Lock::ResourceLock cappedInsertLock(
                    txn->lockState(), resourceCappedInFlight, MODE_X);
                _manager->prepareForCreateSnapshot(txn.get());
            }

            Timestamp thisSnapshot = {};
            {
                AutoGetCollectionForRead oplog(txn.get(), rsOplogName);
                invariant(oplog.getCollection());
                // Read the latest op from the oplog.
                auto record =
                    oplog.getCollection()->getCursor(txn.get(), /*forward*/ false)->next();
                if (!record)
                    continue;  // oplog is completely empty.

                const auto op = record->data.releaseToBson();
                thisSnapshot = op["ts"].timestamp();
                invariant(!thisSnapshot.isNull());
            }

            _manager->createSnapshot(txn.get(), SnapshotName(thisSnapshot));
            _onSnapshotCreate(SnapshotName(thisSnapshot));
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
        invariant(!_inShutdown);
        _inShutdown = true;
        newTimestampNotifier.notify_all();
    }
    _thread.join();
}

void SnapshotThread::forceSnapshot() {
    stdx::lock_guard<stdx::mutex> lock(newOpMutex);
    _forcedSnapshotPending = true;
    newTimestampNotifier.notify_all();
}

std::unique_ptr<SnapshotThread> SnapshotThread::start(ServiceContext* service,
                                                      Callback onSnapshotCreate) {
    auto manager = service->getGlobalStorageEngine()->getSnapshotManager();
    if (!manager)
        return {};
    return std::unique_ptr<SnapshotThread>(
        new SnapshotThread(manager, std::move(onSnapshotCreate)));
}

}  // namespace repl
}  // namespace mongo
