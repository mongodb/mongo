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
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
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
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/platform/random.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/idle_thread_block.h"
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

using IndexVersion = IndexDescriptor::IndexVersion;

namespace repl {
std::string rsOplogName = "local.oplog.rs";
std::string masterSlaveOplogName = "local.oplog.$main";

MONGO_FP_DECLARE(disableSnapshotting);

namespace {
// cached copy...so don't rename, drop, etc.!!!
Collection* _localOplogCollection = nullptr;

PseudoRandom hashGenerator(std::unique_ptr<SecureRandom>(SecureRandom::create())->nextInt64());

// Synchronizes the section where a new Timestamp is generated and when it actually
// appears in the oplog.
stdx::mutex newOpMutex;
stdx::condition_variable newTimestampNotifier;
// Remembers that last timestamp generated for creating new oplog entries or last timestamp of
// oplog entry applied as a secondary. This should only be used for the snapshot thread. Must hold
// the newOpMutex when accessing this variable.
Timestamp lastSetTimestamp;

static std::string _oplogCollectionName;

// so we can fail the same way
void checkOplogInsert(Status result) {
    massert(17322, str::stream() << "write to oplog failed: " << result.toString(), result.isOK());
}

struct OplogSlot {
    OpTime opTime;
    int64_t hash = 0;
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
void getNextOpTime(OperationContext* opCtx,
                   Collection* oplog,
                   ReplicationCoordinator* replCoord,
                   ReplicationCoordinator::Mode replicationMode,
                   unsigned count,
                   OplogSlot* slotsOut) {
    synchronizeOnCappedInFlightResource(opCtx->lockState(), oplog->ns());
    long long term = OpTime::kUninitializedTerm;

    // Fetch term out of the newOpMutex.
    if (replicationMode == ReplicationCoordinator::modeReplSet &&
        replCoord->isV1ElectionProtocol()) {
        // Current term. If we're not a replset of pv=1, it remains kOldProtocolVersionTerm.
        term = replCoord->getTerm();
    }

    stdx::lock_guard<stdx::mutex> lk(newOpMutex);

    auto ts = LogicalClock::get(opCtx)->reserveTicks(count).asTimestamp();
    lastSetTimestamp = ts;
    newTimestampNotifier.notify_all();

    fassert(28560, oplog->getRecordStore()->oplogDiskLocRegister(opCtx, ts));

    // Set hash if we're in replset mode, otherwise it remains 0 in master/slave.
    const bool needHash = (replicationMode == ReplicationCoordinator::modeReplSet);
    for (unsigned i = 0; i < count; i++) {
        slotsOut[i].opTime = {Timestamp(ts.asULL() + i), term};
        if (needHash) {
            slotsOut[i].hash = hashGenerator.nextInt64();
        }
    }
}

/**
 * This allows us to stream the oplog entry directly into data region
 * main goal is to avoid copying the o portion
 * which can be very large
 * TODO: can have this build the entire doc
 */
class OplogDocWriter final : public DocWriter {
public:
    OplogDocWriter(BSONObj frame, BSONObj oField)
        : _frame(std::move(frame)), _oField(std::move(oField)) {}

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

Collection* getLocalOplogCollection(OperationContext* opCtx,
                                    const std::string& oplogCollectionName) {
    if (_localOplogCollection)
        return _localOplogCollection;

    AutoGetCollection autoColl(opCtx, NamespaceString(oplogCollectionName), MODE_IX);
    _localOplogCollection = autoColl.getCollection();
    massert(13347,
            "the oplog collection " + oplogCollectionName +
                " missing. did you drop it? if so, restart the server",
            _localOplogCollection);

    return _localOplogCollection;
}

bool oplogDisabled(OperationContext* opCtx,
                   ReplicationCoordinator::Mode replicationMode,
                   const NamespaceString& nss) {
    if (replicationMode == ReplicationCoordinator::modeNone)
        return true;

    if (nss.db() == "local")
        return true;

    if (nss.isSystemDotProfile())
        return true;

    if (!opCtx->writesAreReplicated())
        return true;

    fassert(28626, opCtx->recoveryUnit());

    return false;
}

OplogDocWriter _logOpWriter(OperationContext* opCtx,
                            const char* opstr,
                            const NamespaceString& nss,
                            OptionalCollectionUUID uuid,
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
    b.append("v", OplogEntry::kOplogVersion);
    b.append("op", opstr);
    b.append("ns", nss.ns());
    if (uuid)
        uuid->appendToBuilder(&b, "ui");
    if (fromMigrate)
        b.appendBool("fromMigrate", true);
    if (o2)
        b.append("o2", *o2);

    return OplogDocWriter(OplogDocWriter(b.obj(), obj));
}
}  // end anon namespace

// Truncates the oplog after and including the "truncateTimestamp" entry.
void truncateOplogTo(OperationContext* opCtx, Timestamp truncateTimestamp) {
    const NamespaceString oplogNss(rsOplogName);
    AutoGetDb autoDb(opCtx, oplogNss.db(), MODE_IX);
    Lock::CollectionLock oplogCollectionLoc(opCtx->lockState(), oplogNss.ns(), MODE_X);
    Collection* oplogCollection = autoDb.getDb()->getCollection(opCtx, oplogNss);
    if (!oplogCollection) {
        fassertFailedWithStatusNoTrace(
            34418,
            Status(ErrorCodes::NamespaceNotFound, str::stream() << "Can't find " << rsOplogName));
    }

    // Scan through oplog in reverse, from latest entry to first, to find the truncateTimestamp.
    RecordId oldestIDToDelete;  // Non-null if there is something to delete.
    auto oplogRs = oplogCollection->getRecordStore();
    auto oplogReverseCursor = oplogRs->getCursor(opCtx, /*forward=*/false);
    size_t count = 0;
    while (auto next = oplogReverseCursor->next()) {
        const BSONObj entry = next->data.releaseToBson();
        const RecordId id = next->id;
        count++;

        const auto tsElem = entry["ts"];
        if (count == 1) {
            if (tsElem.eoo())
                LOG(2) << "Oplog tail entry: " << redact(entry);
            else
                LOG(2) << "Oplog tail entry ts field: " << tsElem;
        }

        if (tsElem.timestamp() < truncateTimestamp) {
            // If count == 1, that means that we have nothing to delete because everything in the
            // oplog is < truncateTimestamp.
            if (count != 1) {
                invariant(!oldestIDToDelete.isNull());
                oplogCollection->cappedTruncateAfter(opCtx, oldestIDToDelete, /*inclusive=*/true);
            }
            return;
        }

        oldestIDToDelete = id;
    }

    severe() << "Reached end of oplog looking for oplog entry before "
             << truncateTimestamp.toStringPretty()
             << " but couldn't find any after looking through " << count << " entries.";
    fassertFailedNoTrace(40296);
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
void _logOpsInner(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const DocWriter* const* writers,
                  size_t nWriters,
                  Collection* oplogCollection,
                  ReplicationCoordinator::Mode replicationMode,
                  OpTime finalOpTime) {
    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();

    if (nss.size() && replicationMode == ReplicationCoordinator::modeReplSet &&
        !replCoord->canAcceptWritesFor(opCtx, nss)) {
        severe() << "logOp() but can't accept write to collection " << nss.ns();
        fassertFailed(17405);
    }

    // we jump through a bunch of hoops here to avoid copying the obj buffer twice --
    // instead we do a single copy to the destination in the record store.
    checkOplogInsert(oplogCollection->insertDocumentsForOplog(opCtx, writers, nWriters));

    // Set replCoord last optime only after we're sure the WUOW didn't abort and roll back.
    opCtx->recoveryUnit()->onCommit([opCtx, replCoord, finalOpTime] {
        replCoord->setMyLastAppliedOpTimeForward(finalOpTime);
        ReplClientInfo::forClient(opCtx->getClient()).setLastOp(finalOpTime);
    });
}

void logOp(OperationContext* opCtx,
           const char* opstr,
           const NamespaceString& nss,
           OptionalCollectionUUID uuid,
           const BSONObj& obj,
           const BSONObj* o2,
           bool fromMigrate) {
    ReplicationCoordinator::Mode replMode =
        ReplicationCoordinator::get(opCtx)->getReplicationMode();
    if (oplogDisabled(opCtx, replMode, nss))
        return;

    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
    Collection* oplog = getLocalOplogCollection(opCtx, _oplogCollectionName);
    Lock::DBLock lk(opCtx, "local", MODE_IX);
    Lock::CollectionLock lock(opCtx->lockState(), _oplogCollectionName, MODE_IX);
    OplogSlot slot;
    getNextOpTime(opCtx, oplog, replCoord, replMode, 1, &slot);
    auto writer =
        _logOpWriter(opCtx, opstr, nss, uuid, obj, o2, fromMigrate, slot.opTime, slot.hash);
    const DocWriter* basePtr = &writer;
    _logOpsInner(opCtx, nss, &basePtr, 1, oplog, replMode, slot.opTime);
}

void logOps(OperationContext* opCtx,
            const char* opstr,
            const NamespaceString& nss,
            OptionalCollectionUUID uuid,
            std::vector<BSONObj>::const_iterator begin,
            std::vector<BSONObj>::const_iterator end,
            bool fromMigrate) {
    ReplicationCoordinator* replCoord = ReplicationCoordinator::get(opCtx);
    ReplicationCoordinator::Mode replMode = replCoord->getReplicationMode();

    invariant(begin != end);
    if (oplogDisabled(opCtx, replMode, nss))
        return;

    const size_t count = end - begin;
    std::vector<OplogDocWriter> writers;
    writers.reserve(count);
    Collection* oplog = getLocalOplogCollection(opCtx, _oplogCollectionName);
    Lock::DBLock lk(opCtx, "local", MODE_IX);
    Lock::CollectionLock lock(opCtx->lockState(), _oplogCollectionName, MODE_IX);
    std::unique_ptr<OplogSlot[]> slots(new OplogSlot[count]);
    getNextOpTime(opCtx, oplog, replCoord, replMode, count, slots.get());
    for (size_t i = 0; i < count; i++) {
        writers.emplace_back(_logOpWriter(
            opCtx, opstr, nss, uuid, begin[i], NULL, fromMigrate, slots[i].opTime, slots[i].hash));
    }

    std::unique_ptr<DocWriter const* []> basePtrs(new DocWriter const*[count]);
    for (size_t i = 0; i < count; i++) {
        basePtrs[i] = &writers[i];
    }
    _logOpsInner(opCtx, nss, basePtrs.get(), count, oplog, replMode, slots[count - 1].opTime);
}

namespace {
long long getNewOplogSizeBytes(OperationContext* opCtx, const ReplSettings& replSettings) {
    if (replSettings.getOplogSizeBytes() != 0) {
        return replSettings.getOplogSizeBytes();
    }
    /* not specified. pick a default size */
    ProcessInfo pi;
    if (pi.getAddrSize() == 32) {
        const auto sz = 50LL * 1024LL * 1024LL;
        LOG(3) << "32bit system; choosing " << sz << " bytes oplog";
        return sz;
    }
// First choose a minimum size.

#if defined(__APPLE__)
    // typically these are desktops (dev machines), so keep it smallish
    const auto sz = 192 * 1024 * 1024;
    LOG(3) << "Apple system; choosing " << sz << " bytes oplog";
    return sz;
#else
    long long lowerBound = 0;
    double bytes = 0;
    if (opCtx->getClient()->getServiceContext()->getGlobalStorageEngine()->isEphemeral()) {
        // in memory: 50MB minimum size
        lowerBound = 50LL * 1024 * 1024;
        bytes = pi.getMemSizeMB() * 1024 * 1024;
        LOG(3) << "Ephemeral storage system; lowerBound: " << lowerBound << " bytes, " << bytes
               << " bytes total memory";
    } else {
        // disk: 990MB minimum size
        lowerBound = 990LL * 1024 * 1024;
        bytes = File::freeSpace(storageGlobalParams.dbpath);  //-1 if call not supported.
        LOG(3) << "Disk storage system; lowerBound: " << lowerBound << " bytes, " << bytes
               << " bytes free space on device";
    }
    long long fivePct = static_cast<long long>(bytes * 0.05);
    auto sz = std::max(fivePct, lowerBound);
    // we use 5% of free [disk] space up to 50GB (1TB free)
    const long long upperBound = 50LL * 1024 * 1024 * 1024;
    sz = std::min(sz, upperBound);
    return sz;
#endif
}
}  // namespace

void createOplog(OperationContext* opCtx, const std::string& oplogCollectionName, bool isReplSet) {
    Lock::GlobalWrite lk(opCtx);

    const ReplSettings& replSettings = ReplicationCoordinator::get(opCtx)->getSettings();

    OldClientContext ctx(opCtx, oplogCollectionName);
    Collection* collection = ctx.db()->getCollection(opCtx, oplogCollectionName);

    if (collection) {
        if (replSettings.getOplogSizeBytes() != 0) {
            const CollectionOptions oplogOpts =
                collection->getCatalogEntry()->getCollectionOptions(opCtx);

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

        if (!isReplSet)
            initTimestampFromOplog(opCtx, oplogCollectionName);
        return;
    }

    /* create an oplog collection, if it doesn't yet exist. */
    const auto sz = getNewOplogSizeBytes(opCtx, replSettings);

    log() << "******" << endl;
    log() << "creating replication oplog of size: " << (int)(sz / (1024 * 1024)) << "MB..." << endl;

    CollectionOptions options;
    options.capped = true;
    options.cappedSize = sz;
    options.autoIndexId = CollectionOptions::NO;

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        WriteUnitOfWork uow(opCtx);
        invariant(ctx.db()->createCollection(opCtx, oplogCollectionName, options));
        if (!isReplSet)
            getGlobalServiceContext()->getOpObserver()->onOpMessage(opCtx, BSONObj());
        uow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "createCollection", oplogCollectionName);

    /* sync here so we don't get any surprising lag later when we try to sync */
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
    storageEngine->flushAllFiles(opCtx, true);
    log() << "******" << endl;
}

void createOplog(OperationContext* opCtx) {
    const auto isReplSet = ReplicationCoordinator::get(opCtx)->getReplicationMode() ==
        ReplicationCoordinator::modeReplSet;
    createOplog(opCtx, _oplogCollectionName, isReplSet);
}

// -------------------------------------

namespace {
NamespaceString parseNs(const string& ns, const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    uassert(40073,
            str::stream() << "collection name has invalid type " << typeName(first.type()),
            first.canonicalType() == canonicalizeBSONType(mongo::String));
    std::string coll = first.valuestr();
    uassert(28635, "no collection name specified", !coll.empty());
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
     {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
          const NamespaceString nss(parseNs(ns, cmd));
          if (auto idIndexElem = cmd["idIndex"]) {
              // Remove "idIndex" field from command.
              auto cmdWithoutIdIndex = cmd.removeField("idIndex");
              return createCollection(
                  opCtx, nss.db().toString(), cmdWithoutIdIndex, idIndexElem.Obj());
          }

          // No _id index spec was provided, so we should build a v:1 _id index.
          BSONObjBuilder idIndexSpecBuilder;
          idIndexSpecBuilder.append(IndexDescriptor::kIndexVersionFieldName,
                                    static_cast<int>(IndexVersion::kV1));
          idIndexSpecBuilder.append(IndexDescriptor::kIndexNameFieldName, "_id_");
          idIndexSpecBuilder.append(IndexDescriptor::kNamespaceFieldName, nss.ns());
          idIndexSpecBuilder.append(IndexDescriptor::kKeyPatternFieldName, BSON("_id" << 1));
          return createCollection(opCtx, nss.db().toString(), cmd, idIndexSpecBuilder.done());
      },
      {ErrorCodes::NamespaceExists}}},
    {"collMod",
     {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return collMod(opCtx, parseNs(ns, cmd), cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::IndexNotFound, ErrorCodes::NamespaceNotFound}}},
    {"dropDatabase",
     {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
          return dropDatabase(opCtx, NamespaceString(ns).db().toString());
      },
      {ErrorCodes::NamespaceNotFound}}},
    {"drop",
     {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return dropCollection(opCtx, parseNs(ns, cmd), resultWeDontCareAbout);
      },
      // IllegalOperation is necessary because in 3.0 we replicate drops of system.profile
      // TODO(dannenberg) remove IllegalOperation once we no longer need 3.0 compatibility
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IllegalOperation}}},
    // deleteIndex(es) is deprecated but still works as of April 10, 2015
    {"deleteIndex",
     {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return dropIndexes(opCtx, parseNs(ns, cmd), cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"deleteIndexes",
     {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return dropIndexes(opCtx, parseNs(ns, cmd), cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"dropIndex",
     {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return dropIndexes(opCtx, parseNs(ns, cmd), cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"dropIndexes",
     {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return dropIndexes(opCtx, parseNs(ns, cmd), cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::IndexNotFound}}},
    {"renameCollection",
     {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
          const auto sourceNsElt = cmd.firstElement();
          const auto targetNsElt = cmd["to"];
          uassert(ErrorCodes::TypeMismatch,
                  "'renameCollection' must be of type String",
                  sourceNsElt.type() == BSONType::String);
          uassert(ErrorCodes::TypeMismatch,
                  "'to' must be of type String",
                  targetNsElt.type() == BSONType::String);
          return renameCollection(opCtx,
                                  NamespaceString(sourceNsElt.valueStringData()),
                                  NamespaceString(targetNsElt.valueStringData()),
                                  cmd["dropTarget"].trueValue(),
                                  cmd["stayTemp"].trueValue());
      },
      {ErrorCodes::NamespaceNotFound, ErrorCodes::NamespaceExists}}},
    {"applyOps",
     {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
          BSONObjBuilder resultWeDontCareAbout;
          return applyOps(opCtx, nsToDatabase(ns), cmd, &resultWeDontCareAbout);
      },
      {ErrorCodes::UnknownError}}},
    {"convertToCapped", {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
         return convertToCapped(opCtx, parseNs(ns, cmd), cmd["size"].number());
     }}},
    {"emptycapped", {[](OperationContext* opCtx, const char* ns, BSONObj& cmd) -> Status {
         return emptyCapped(opCtx, parseNs(ns, cmd));
     }}},
};

}  // namespace

// @return failure status if an update should have happened and the document DNE.
// See replset initial sync code.
Status applyOperation_inlock(OperationContext* opCtx,
                             Database* db,
                             const BSONObj& op,
                             bool inSteadyStateReplication,
                             IncrementOpsAppliedStatsFn incrementOpsAppliedStats) {
    LOG(3) << "applying op: " << redact(op);

    OpCounters* opCounters = opCtx->writesAreReplicated() ? &globalOpCounters : &replOpCounters;

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

    uassert(ErrorCodes::InvalidNamespace,
            "'ns' must be of type String",
            fieldNs.type() == BSONType::String);
    const StringData ns = fieldNs.valueStringData();
    NamespaceString requestNss{ns};

    BSONObj o2;
    if (fieldO2.isABSONObj())
        o2 = fieldO2.Obj();

    bool valueB = fieldB.booleanSafe();

    if (nsIsFull(ns)) {
        if (supportsDocLocking()) {
            // WiredTiger, and others requires MODE_IX since the applier threads driving
            // this allow writes to the same collection on any thread.
            dassert(opCtx->lockState()->isCollectionLockedForMode(ns, MODE_IX));
        } else {
            // mmapV1 ensures that all operations to the same collection are executed from
            // the same worker thread, so it takes an exclusive lock (MODE_X)
            dassert(opCtx->lockState()->isCollectionLockedForMode(ns, MODE_X));
        }
    }
    Collection* collection = db->getCollection(opCtx, requestNss);
    IndexCatalog* indexCatalog = collection == nullptr ? nullptr : collection->getIndexCatalog();
    const bool haveWrappingWriteUnitOfWork = opCtx->lockState()->inAWriteUnitOfWork();
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "applyOps not supported on view: " << ns,
            collection || !db->getViewCatalog()->lookup(opCtx, ns));

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
            BSONObj indexSpec = fieldO.embeddedObject();

            std::string indexNs;
            uassertStatusOK(bsonExtractStringField(indexSpec, "ns", &indexNs));
            const NamespaceString indexNss(indexNs);
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid namespace in index spec: " << op,
                    indexNss.isValid());
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Database name mismatch for database ("
                                  << nsToDatabaseSubstring(ns)
                                  << ") while creating index: "
                                  << op,
                    nsToDatabaseSubstring(ns) == indexNss.db());

            // Check if collection exists.
            auto indexCollection = db->getCollection(opCtx, indexNss);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Failed to create index due to missing collection: "
                                  << op.toString(),
                    indexCollection);

            opCounters->gotInsert();

            if (!indexSpec["v"]) {
                // If the "v" field isn't present in the index specification, then we assume it is a
                // v=1 index from an older version of MongoDB. This is because
                //   (1) we haven't built v=0 indexes as the default for a long time, and
                //   (2) the index version has been included in the corresponding oplog entry since
                //       v=2 indexes were introduced.
                BSONObjBuilder bob;

                bob.append("v", static_cast<int>(IndexVersion::kV1));
                bob.appendElements(indexSpec);

                indexSpec = bob.obj();
            }

            bool relaxIndexConstraints =
                ReplicationCoordinator::get(opCtx)->shouldRelaxIndexConstraints(opCtx, indexNss);
            if (indexSpec["background"].trueValue()) {
                Lock::TempRelease release(opCtx->lockState());
                if (opCtx->lockState()->isLocked()) {
                    // If TempRelease fails, background index build will deadlock.
                    LOG(3) << "apply op: building background index " << indexSpec
                           << " in the foreground because temp release failed";
                    IndexBuilder builder(indexSpec, relaxIndexConstraints);
                    Status status = builder.buildInForeground(opCtx, db);
                    uassertStatusOK(status);
                } else {
                    IndexBuilder* builder = new IndexBuilder(indexSpec, relaxIndexConstraints);
                    // This spawns a new thread and returns immediately.
                    builder->go();
                    // Wait for thread to start and register itself
                    IndexBuilder::waitForBgIndexStarting();
                }
                opCtx->recoveryUnit()->abandonSnapshot();
            } else {
                IndexBuilder builder(indexSpec, relaxIndexConstraints);
                Status status = builder.buildInForeground(opCtx, db);
                uassertStatusOK(status);
            }
            // Since this is an index operation we can return without falling through.
            if (incrementOpsAppliedStats) {
                incrementOpsAppliedStats();
            }
            return Status::OK();
        }
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Failed to apply insert due to missing collection: "
                              << op.toString(),
                collection);

        if (fieldO.type() == Array) {
            // Batched inserts.
            std::vector<BSONObj> insertObjs;
            for (auto elem : fieldO.Obj()) {
                insertObjs.push_back(elem.Obj());
            }
            uassert(ErrorCodes::OperationFailed,
                    str::stream() << "Failed to apply insert due to empty array element: "
                                  << op.toString(),
                    !insertObjs.empty());
            WriteUnitOfWork wuow(opCtx);
            OpDebug* const nullOpDebug = nullptr;
            Status status = collection->insertDocuments(
                opCtx, insertObjs.begin(), insertObjs.end(), nullOpDebug, true);
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

            // 1. Try insert first, if we have no wrappingWriteUnitOfWork
            // 2. If okay, commit
            // 3. If not, do upsert (and commit)
            // 4. If both !Ok, return status
            Status status{ErrorCodes::NotYetInitialized, ""};

            // We cannot rely on a DuplicateKey error if we'repart of a larger transaction, because
            // that would require the transaction to abort. So instead, use upsert in that case.
            bool needToDoUpsert = haveWrappingWriteUnitOfWork;

            if (!needToDoUpsert) {
                WriteUnitOfWork wuow(opCtx);
                try {
                    OpDebug* const nullOpDebug = nullptr;
                    status = collection->insertDocument(opCtx, o, nullOpDebug, true);
                } catch (DBException dbe) {
                    status = dbe.toStatus();
                }
                if (status.isOK()) {
                    wuow.commit();
                } else if (status == ErrorCodes::DuplicateKey) {
                    needToDoUpsert = true;
                } else {
                    return status;
                }
            }
            // Now see if we need to do an upsert.
            if (needToDoUpsert) {
                // Do update on DuplicateKey errors.
                // This will only be on the _id field in replication,
                // since we disable non-_id unique constraint violations.
                BSONObjBuilder b;
                b.append(o.getField("_id"));

                UpdateRequest request(requestNss);

                request.setQuery(b.done());
                request.setUpdates(o);
                request.setUpsert();
                UpdateLifecycleImpl updateLifecycle(requestNss);
                request.setLifecycle(&updateLifecycle);

                UpdateResult res = update(opCtx, db, request);
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

        BSONObj updateCriteria = o2;
        const bool upsert = valueB || inSteadyStateReplication;

        uassert(ErrorCodes::NoSuchKey,
                str::stream() << "Failed to apply update due to missing _id: " << op.toString(),
                updateCriteria.hasField("_id"));

        UpdateRequest request(requestNss);

        request.setQuery(updateCriteria);
        request.setUpdates(o);
        request.setUpsert(upsert);
        UpdateLifecycleImpl updateLifecycle(requestNss);
        request.setLifecycle(&updateLifecycle);

        UpdateResult ur = update(opCtx, db, request);

        if (ur.numMatched == 0 && ur.upserted.isEmpty()) {
            if (ur.modifiers) {
                if (updateCriteria.nFields() == 1) {
                    // was a simple { _id : ... } update criteria
                    string msg = str::stream() << "failed to apply update: " << redact(op);
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
                    (indexCatalog->haveIdIndex(opCtx) &&
                     Helpers::findById(opCtx, collection, updateCriteria).isNull()) ||
                    // capped collections won't have an _id index
                    (!indexCatalog->haveIdIndex(opCtx) &&
                     Helpers::findOne(opCtx, collection, updateCriteria, false).isNull())) {
                    string msg = str::stream() << "couldn't find doc: " << redact(op);
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
                    string msg = str::stream() << "update of non-mod failed: " << redact(op);
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
            deleteObjects(opCtx, collection, requestNss, o, /*justOne*/ valueB);
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

    // AuthorizationManager's logOp method registers a RecoveryUnit::Change and to do so we need
    // to a new WriteUnitOfWork, if we dont have a wrapping unit of work already. If we already
    // have a wrapping WUOW, the extra nexting is harmless. The logOp really should have been
    // done in the WUOW that did the write, but this won't happen because applyOps turns off
    // observers.
    WriteUnitOfWork wuow(opCtx);
    getGlobalAuthorizationManager()->logOp(
        opCtx, opType, requestNss, o, fieldO2.isABSONObj() ? &o2 : NULL);
    wuow.commit();

    return Status::OK();
}

Status applyCommand_inlock(OperationContext* opCtx,
                           const BSONObj& op,
                           bool inSteadyStateReplication) {
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

    uassert(ErrorCodes::InvalidNamespace,
            "'ns' must be of type String",
            fieldNs.type() == BSONType::String);
    const NamespaceString nss(fieldNs.valueStringData());
    if (!nss.isValid()) {
        return {ErrorCodes::InvalidNamespace, "invalid ns: " + std::string(nss.ns())};
    }
    {
        Database* db = dbHolder().get(opCtx, nss.ns());
        if (db && !db->getCollection(opCtx, nss) && db->getViewCatalog()->lookup(opCtx, nss.ns())) {
            return {ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "applyOps not supported on view:" << nss.ns()};
        }
    }

    // Applying renameCollection during initial sync might lead to data corruption, so we restart
    // the initial sync.
    if (!inSteadyStateReplication && o.firstElementFieldName() == std::string("renameCollection")) {
        return Status(ErrorCodes::OplogOperationUnsupported,
                      str::stream() << "Applying renameCollection not supported in initial sync: "
                                    << redact(op));
    }

    // Applying commands in repl is done under Global W-lock, so it is safe to not
    // perform the current DB checks after reacquiring the lock.
    invariant(opCtx->lockState()->isW());

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
            status = curOpToApply.applyFunc(opCtx, nss.ns().c_str(), o);
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
                Lock::TempRelease release(opCtx->lockState());

                BackgroundOperation::awaitNoBgOpInProgForDb(nss.db());
                opCtx->recoveryUnit()->abandonSnapshot();
                opCtx->checkForInterrupt();
                break;
            }
            case ErrorCodes::BackgroundOperationInProgressForNamespace: {
                Lock::TempRelease release(opCtx->lockState());

                Command* cmd = Command::findCommand(o.firstElement().fieldName());
                invariant(cmd);
                BackgroundOperation::awaitNoBgOpInProgForNs(cmd->parseNs(nss.db().toString(), o));
                opCtx->recoveryUnit()->abandonSnapshot();
                opCtx->checkForInterrupt();
                break;
            }
            default:
                if (_oplogCollectionName == masterSlaveOplogName) {
                    error() << "Failed command " << redact(o) << " on " << nss.db()
                            << " with status " << status << " during oplog application";
                } else if (curOpToApply.acceptableErrors.find(status.code()) ==
                           curOpToApply.acceptableErrors.end()) {
                    error() << "Failed command " << redact(o) << " on " << nss.db()
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
    WriteUnitOfWork wuow(opCtx);
    getGlobalAuthorizationManager()->logOp(opCtx, opType, nss, o, nullptr);
    wuow.commit();

    return Status::OK();
}

void setNewTimestamp(ServiceContext* service, const Timestamp& newTime) {
    stdx::lock_guard<stdx::mutex> lk(newOpMutex);
    LogicalClock::get(service)->setClusterTimeFromTrustedSource(LogicalTime(newTime));
    lastSetTimestamp = newTime;
    newTimestampNotifier.notify_all();
}

void initTimestampFromOplog(OperationContext* opCtx, const std::string& oplogNS) {
    DBDirectClient c(opCtx);
    BSONObj lastOp = c.findOne(oplogNS, Query().sort(reverseNaturalObj), NULL, QueryOption_SlaveOk);

    if (!lastOp.isEmpty()) {
        LOG(1) << "replSet setting last Timestamp";
        const OpTime opTime = fassertStatusOK(28696, OpTime::parseFromOplogEntry(lastOp));
        setNewTimestamp(opCtx->getServiceContext(), opTime.getTimestamp());
    }
}

void oplogCheckCloseDatabase(OperationContext* opCtx, Database* db) {
    invariant(opCtx->lockState()->isW());

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
    auto service = client.getServiceContext();
    auto replCoord = ReplicationCoordinator::get(service);

    Timestamp lastTimestamp(Timestamp::max());  // hack to trigger snapshot from startup.
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

                if (_forcedSnapshotPending.load() || lastTimestamp != lastSetTimestamp) {
                    _forcedSnapshotPending.store(false);
                    lastTimestamp = lastSetTimestamp;
                    break;
                }

                MONGO_IDLE_THREAD_BLOCK;
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
            auto opCtx = client.makeOperationContext();
            Lock::GlobalLock globalLock(opCtx.get(), MODE_IS, UINT_MAX);

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
                // This lock cannot be aquired until all writes holding the resource commit/abort.
                Lock::ResourceLock cappedInsertLockForOtherDb(
                    opCtx->lockState(), resourceCappedInFlightForOtherDb, MODE_X);
                Lock::ResourceLock cappedInsertLockForLocalDb(
                    opCtx->lockState(), resourceCappedInFlightForLocalDb, MODE_X);

                // Reserve the name immediately before we take our snapshot. This ensures that all
                // names that compare lower must be from points in time visible to this named
                // snapshot.
                name = replCoord->reserveSnapshotName(nullptr);

                // This establishes the view that we will name.
                _manager->prepareForCreateSnapshot(opCtx.get());
            }

            auto opTimeOfSnapshot = OpTime();
            {
                AutoGetCollectionForReadCommand oplog(opCtx.get(), NamespaceString(rsOplogName));
                invariant(oplog.getCollection());
                // Read the latest op from the oplog.
                auto cursor = oplog.getCollection()->getCursor(opCtx.get(), /*forward*/ false);
                auto record = cursor->next();
                if (!record)
                    continue;  // oplog is completely empty.

                const auto op = record->data.releaseToBson();
                opTimeOfSnapshot = fassertStatusOK(28780, OpTime::parseFromOplogEntry(op));
                invariant(!opTimeOfSnapshot.isNull());
            }

            replCoord->createSnapshot(opCtx.get(), opTimeOfSnapshot, name);
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
