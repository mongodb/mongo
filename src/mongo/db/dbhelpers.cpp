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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/dbhelpers.h"

#include <boost/filesystem/operations.hpp>
#include <fstream>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/json.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/data_protector.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::unique_ptr;
using std::endl;
using std::ios_base;
using std::ofstream;
using std::set;
using std::string;
using std::stringstream;

using logger::LogComponent;

void Helpers::ensureIndex(OperationContext* txn,
                          Collection* collection,
                          BSONObj keyPattern,
                          bool unique,
                          const char* name) {
    BSONObjBuilder b;
    b.append("name", name);
    b.append("ns", collection->ns().ns());
    b.append("key", keyPattern);
    b.appendBool("unique", unique);
    BSONObj o = b.done();

    MultiIndexBlock indexer(txn, collection);

    Status status = indexer.init(o);
    if (status.code() == ErrorCodes::IndexAlreadyExists)
        return;
    uassertStatusOK(status);

    uassertStatusOK(indexer.insertAllDocumentsInCollection());

    WriteUnitOfWork wunit(txn);
    indexer.commit();
    wunit.commit();
}

/* fetch a single object from collection ns that matches query
   set your db SavedContext first
*/
bool Helpers::findOne(OperationContext* txn,
                      Collection* collection,
                      const BSONObj& query,
                      BSONObj& result,
                      bool requireIndex) {
    RecordId loc = findOne(txn, collection, query, requireIndex);
    if (loc.isNull())
        return false;
    result = collection->docFor(txn, loc).value();
    return true;
}

/* fetch a single object from collection ns that matches query
   set your db SavedContext first
*/
RecordId Helpers::findOne(OperationContext* txn,
                          Collection* collection,
                          const BSONObj& query,
                          bool requireIndex) {
    if (!collection)
        return RecordId();

    const ExtensionsCallbackReal extensionsCallback(txn, &collection->ns());

    auto qr = stdx::make_unique<QueryRequest>(collection->ns());
    qr->setFilter(query);

    auto statusWithCQ = CanonicalQuery::canonicalize(txn, std::move(qr), extensionsCallback);
    massert(17244, "Could not canonicalize " + query.toString(), statusWithCQ.isOK());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    size_t options = requireIndex ? QueryPlannerParams::NO_TABLE_SCAN : QueryPlannerParams::DEFAULT;
    auto statusWithPlanExecutor =
        getExecutor(txn, collection, std::move(cq), PlanExecutor::YIELD_MANUAL, options);
    massert(17245,
            "Could not get executor for query " + query.toString(),
            statusWithPlanExecutor.isOK());

    unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());
    PlanExecutor::ExecState state;
    BSONObj obj;
    RecordId loc;
    if (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, &loc))) {
        return loc;
    }
    massert(34427,
            "Plan executor error: " + WorkingSetCommon::toStatusString(obj),
            PlanExecutor::IS_EOF == state);
    return RecordId();
}

bool Helpers::findById(OperationContext* txn,
                       Database* database,
                       const char* ns,
                       BSONObj query,
                       BSONObj& result,
                       bool* nsFound,
                       bool* indexFound) {
    invariant(database);

    Collection* collection = database->getCollection(ns);
    if (!collection) {
        return false;
    }

    if (nsFound)
        *nsFound = true;

    IndexCatalog* catalog = collection->getIndexCatalog();
    const IndexDescriptor* desc = catalog->findIdIndex(txn);

    if (!desc)
        return false;

    if (indexFound)
        *indexFound = 1;

    RecordId loc = catalog->getIndex(desc)->findSingle(txn, query["_id"].wrap());
    if (loc.isNull())
        return false;
    result = collection->docFor(txn, loc).value();
    return true;
}

RecordId Helpers::findById(OperationContext* txn, Collection* collection, const BSONObj& idquery) {
    verify(collection);
    IndexCatalog* catalog = collection->getIndexCatalog();
    const IndexDescriptor* desc = catalog->findIdIndex(txn);
    uassert(13430, "no _id index", desc);
    return catalog->getIndex(desc)->findSingle(txn, idquery["_id"].wrap());
}

bool Helpers::getSingleton(OperationContext* txn, const char* ns, BSONObj& result) {
    AutoGetCollectionForRead ctx(txn, ns);
    unique_ptr<PlanExecutor> exec(
        InternalPlanner::collectionScan(txn, ns, ctx.getCollection(), PlanExecutor::YIELD_MANUAL));
    PlanExecutor::ExecState state = exec->getNext(&result, NULL);

    CurOp::get(txn)->done();

    // Non-yielding collection scans from InternalPlanner will never error.
    invariant(PlanExecutor::ADVANCED == state || PlanExecutor::IS_EOF == state);

    if (PlanExecutor::ADVANCED == state) {
        result = result.getOwned();
        return true;
    }

    return false;
}

bool Helpers::getLast(OperationContext* txn, const char* ns, BSONObj& result) {
    AutoGetCollectionForRead autoColl(txn, ns);
    unique_ptr<PlanExecutor> exec(InternalPlanner::collectionScan(
        txn, ns, autoColl.getCollection(), PlanExecutor::YIELD_MANUAL, InternalPlanner::BACKWARD));
    PlanExecutor::ExecState state = exec->getNext(&result, NULL);

    // Non-yielding collection scans from InternalPlanner will never error.
    invariant(PlanExecutor::ADVANCED == state || PlanExecutor::IS_EOF == state);

    if (PlanExecutor::ADVANCED == state) {
        result = result.getOwned();
        return true;
    }

    return false;
}

void Helpers::upsert(OperationContext* txn, const string& ns, const BSONObj& o, bool fromMigrate) {
    BSONElement e = o["_id"];
    verify(e.type());
    BSONObj id = e.wrap();

    OldClientContext context(txn, ns);

    const NamespaceString requestNs(ns);
    UpdateRequest request(requestNs);

    request.setQuery(id);
    request.setUpdates(o);
    request.setUpsert();
    request.setFromMigration(fromMigrate);
    UpdateLifecycleImpl updateLifecycle(requestNs);
    request.setLifecycle(&updateLifecycle);

    update(txn, context.db(), request);
}

void Helpers::putSingleton(OperationContext* txn, const char* ns, BSONObj obj) {
    OldClientContext context(txn, ns);

    const NamespaceString requestNs(ns);
    UpdateRequest request(requestNs);

    request.setUpdates(obj);
    request.setUpsert();
    UpdateLifecycleImpl updateLifecycle(requestNs);
    request.setLifecycle(&updateLifecycle);

    update(txn, context.db(), request);

    CurOp::get(txn)->done();
}

BSONObj Helpers::toKeyFormat(const BSONObj& o) {
    BSONObjBuilder keyObj(o.objsize());
    BSONForEach(e, o) {
        keyObj.appendAs(e, "");
    }
    return keyObj.obj();
}

BSONObj Helpers::inferKeyPattern(const BSONObj& o) {
    BSONObjBuilder kpBuilder;
    BSONForEach(e, o) {
        kpBuilder.append(e.fieldName(), 1);
    }
    return kpBuilder.obj();
}

static bool findShardKeyIndexPattern(OperationContext* txn,
                                     const string& ns,
                                     const BSONObj& shardKeyPattern,
                                     BSONObj* indexPattern) {
    AutoGetCollectionForRead ctx(txn, ns);
    Collection* collection = ctx.getCollection();
    if (!collection) {
        return false;
    }

    // Allow multiKey based on the invariant that shard keys must be single-valued.
    // Therefore, any multi-key index prefixed by shard key cannot be multikey over
    // the shard key fields.
    const IndexDescriptor* idx =
        collection->getIndexCatalog()->findShardKeyPrefixedIndex(txn,
                                                                 shardKeyPattern,
                                                                 false);  // requireSingleKey

    if (idx == NULL)
        return false;
    *indexPattern = idx->keyPattern().getOwned();
    return true;
}

long long Helpers::removeRange(OperationContext* txn,
                               const KeyRange& range,
                               bool maxInclusive,
                               const WriteConcernOptions& writeConcern,
                               RemoveSaver* callback,
                               bool fromMigrate,
                               bool onlyRemoveOrphanedDocs) {
    Timer rangeRemoveTimer;
    const string& ns = range.ns;

    // The IndexChunk has a keyPattern that may apply to more than one index - we need to
    // select the index and get the full index keyPattern here.
    BSONObj indexKeyPatternDoc;
    if (!findShardKeyIndexPattern(txn, ns, range.keyPattern, &indexKeyPatternDoc)) {
        warning(LogComponent::kSharding) << "no index found to clean data over range of type "
                                         << range.keyPattern << " in " << ns << endl;
        return -1;
    }

    KeyPattern indexKeyPattern(indexKeyPatternDoc);

    // Extend bounds to match the index we found

    // Extend min to get (min, MinKey, MinKey, ....)
    const BSONObj& min =
        Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(range.minKey, false));
    // If upper bound is included, extend max to get (max, MaxKey, MaxKey, ...)
    // If not included, extend max to get (max, MinKey, MinKey, ....)
    const BSONObj& max =
        Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(range.maxKey, maxInclusive));

    MONGO_LOG_COMPONENT(1, LogComponent::kSharding)
        << "begin removal of " << min << " to " << max << " in " << ns
        << " with write concern: " << writeConcern.toBSON() << endl;

    long long numDeleted = 0;

    Milliseconds millisWaitingForReplication{0};

    while (1) {
        // Scoping for write lock.
        {
            OldClientWriteContext ctx(txn, ns);
            Collection* collection = ctx.getCollection();
            if (!collection)
                break;

            IndexDescriptor* desc =
                collection->getIndexCatalog()->findIndexByKeyPattern(txn, indexKeyPattern.toBSON());

            unique_ptr<PlanExecutor> exec(
                InternalPlanner::indexScan(txn,
                                           collection,
                                           desc,
                                           min,
                                           max,
                                           maxInclusive,
                                           PlanExecutor::YIELD_MANUAL,
                                           InternalPlanner::FORWARD,
                                           InternalPlanner::IXSCAN_FETCH));
            exec->setYieldPolicy(PlanExecutor::YIELD_AUTO, collection);

            RecordId rloc;
            BSONObj obj;
            PlanExecutor::ExecState state;
            // This may yield so we cannot touch nsd after this.
            state = exec->getNext(&obj, &rloc);
            if (PlanExecutor::IS_EOF == state) {
                break;
            }

            if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
                warning(LogComponent::kSharding)
                    << PlanExecutor::statestr(state) << " - cursor error while trying to delete "
                    << min << " to " << max << " in " << ns << ": "
                    << WorkingSetCommon::toStatusString(obj)
                    << ", stats: " << Explain::getWinningPlanStats(exec.get()) << endl;
                break;
            }

            verify(PlanExecutor::ADVANCED == state);

            WriteUnitOfWork wuow(txn);

            if (onlyRemoveOrphanedDocs) {
                // Do a final check in the write lock to make absolutely sure that our
                // collection hasn't been modified in a way that invalidates our migration
                // cleanup.

                // We should never be able to turn off the sharding state once enabled, but
                // in the future we might want to.
                verify(ShardingState::get(txn)->enabled());

                bool docIsOrphan;

                // In write lock, so will be the most up-to-date version
                auto metadataNow = CollectionShardingState::get(txn, ns)->getMetadata();
                if (metadataNow) {
                    ShardKeyPattern kp(metadataNow->getKeyPattern());
                    BSONObj key = kp.extractShardKeyFromDoc(obj);
                    docIsOrphan =
                        !metadataNow->keyBelongsToMe(key) && !metadataNow->keyIsPending(key);
                } else {
                    docIsOrphan = false;
                }

                if (!docIsOrphan) {
                    warning(LogComponent::kSharding)
                        << "aborting migration cleanup for chunk " << min << " to " << max
                        << (metadataNow ? (string) " at document " + obj.toString() : "")
                        << ", collection " << ns << " has changed " << endl;
                    break;
                }
            }

            NamespaceString nss(ns);
            if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nss)) {
                warning() << "stepped down from primary while deleting chunk; "
                          << "orphaning data in " << ns << " in range [" << min << ", " << max
                          << ")";
                return numDeleted;
            }

            if (callback)
                callback->goingToDelete(obj);

            OpDebug* const nullOpDebug = nullptr;
            collection->deleteDocument(txn, rloc, nullOpDebug, fromMigrate);
            wuow.commit();
            numDeleted++;
        }

        // TODO remove once the yielding below that references this timer has been removed
        Timer secondaryThrottleTime;

        if (writeConcern.shouldWaitForOtherNodes() && numDeleted > 0) {
            repl::ReplicationCoordinator::StatusAndDuration replStatus =
                repl::getGlobalReplicationCoordinator()->awaitReplication(
                    txn,
                    repl::ReplClientInfo::forClient(txn->getClient()).getLastOp(),
                    writeConcern);
            if (replStatus.status.code() == ErrorCodes::ExceededTimeLimit) {
                warning(LogComponent::kSharding) << "replication to secondaries for removeRange at "
                                                    "least 60 seconds behind";
            } else {
                uassertStatusOK(replStatus.status);
            }
            millisWaitingForReplication += replStatus.duration;
        }
    }

    if (writeConcern.shouldWaitForOtherNodes())
        log(LogComponent::kSharding)
            << "Helpers::removeRangeUnlocked time spent waiting for replication: "
            << durationCount<Milliseconds>(millisWaitingForReplication) << "ms" << endl;

    MONGO_LOG_COMPONENT(1, LogComponent::kSharding) << "end removal of " << min << " to " << max
                                                    << " in " << ns << " (took "
                                                    << rangeRemoveTimer.millis() << "ms)" << endl;

    return numDeleted;
}

void Helpers::emptyCollection(OperationContext* txn, const char* ns) {
    OldClientContext context(txn, ns);
    bool shouldReplicateWrites = txn->writesAreReplicated();
    txn->setReplicatedWrites(false);
    ON_BLOCK_EXIT(&OperationContext::setReplicatedWrites, txn, shouldReplicateWrites);
    Collection* collection = context.db() ? context.db()->getCollection(ns) : nullptr;
    deleteObjects(txn, collection, ns, BSONObj(), PlanExecutor::YIELD_MANUAL, false);
}

Helpers::RemoveSaver::RemoveSaver(const string& a, const string& b, const string& why) {
    static int NUM = 0;

    _root = storageGlobalParams.dbpath;
    if (a.size())
        _root /= a;
    if (b.size())
        _root /= b;
    verify(a.size() || b.size());

    _file = _root;

    stringstream ss;
    ss << why << "." << terseCurrentTime(false) << "." << NUM++ << ".bson";
    _file /= ss.str();

    auto hooks = WiredTigerCustomizationHooks::get(getGlobalServiceContext());
    if (hooks->enabled()) {
        _protector = hooks->getDataProtector();
        _file += hooks->getProtectedPathSuffix();
    }
}

Helpers::RemoveSaver::~RemoveSaver() {
    if (_protector && _out) {
        auto hooks = WiredTigerCustomizationHooks::get(getGlobalServiceContext());
        invariant(hooks->enabled());

        size_t protectedSizeMax = hooks->additionalBytesForProtectedBuffer();
        std::unique_ptr<uint8_t[]> protectedBuffer(new uint8_t[protectedSizeMax]);

        size_t resultLen;
        Status status = _protector->finalize(protectedBuffer.get(), protectedSizeMax, &resultLen);
        if (!status.isOK()) {
            severe() << "Unable to finalize DataProtector while closing RemoveSaver: "
                     << status.reason();
            fassertFailed(34350);
        }

        _out->write(reinterpret_cast<const char*>(protectedBuffer.get()), resultLen);
        if (_out->fail()) {
            severe() << "Couldn't write finalized DataProtector data to: " << _file.string()
                     << " for remove saving: " << errnoWithDescription();
            fassertFailed(34351);
        }

        protectedBuffer.reset(new uint8_t[protectedSizeMax]);
        status = _protector->finalizeTag(protectedBuffer.get(), protectedSizeMax, &resultLen);
        if (!status.isOK()) {
            severe() << "Unable to get finalizeTag from DataProtector while closing RemoveSaver: "
                     << status.reason();
            fassertFailed(34352);
        }
        if (resultLen != _protector->getNumberOfBytesReservedForTag()) {
            severe() << "Attempted to write tag of size " << resultLen
                     << " when DataProtector only reserved "
                     << _protector->getNumberOfBytesReservedForTag() << " bytes";
            fassertFailed(34353);
        }
        _out->seekp(0);
        _out->write(reinterpret_cast<const char*>(protectedBuffer.get()), resultLen);
        if (_out->fail()) {
            severe() << "Couldn't write finalizeTag from DataProtector to: " << _file.string()
                     << " for remove saving: " << errnoWithDescription();
            fassertFailed(34354);
        }
    }
}

Status Helpers::RemoveSaver::goingToDelete(const BSONObj& o) {
    if (!_out) {
        boost::filesystem::create_directories(_root);
        _out.reset(new ofstream(_file.string().c_str(), ios_base::out | ios_base::binary));
        if (_out->fail()) {
            string msg = str::stream() << "couldn't create file: " << _file.string()
                                       << " for remove saving: " << errnoWithDescription();
            error() << msg;
            _out.reset();
            _out = 0;
            return Status(ErrorCodes::FileNotOpen, msg);
        }
    }

    const uint8_t* data = reinterpret_cast<const uint8_t*>(o.objdata());
    size_t dataSize = o.objsize();

    std::unique_ptr<uint8_t[]> protectedBuffer;
    if (_protector) {
        auto hooks = WiredTigerCustomizationHooks::get(getGlobalServiceContext());
        invariant(hooks->enabled());

        size_t protectedSizeMax = dataSize + hooks->additionalBytesForProtectedBuffer();
        protectedBuffer.reset(new uint8_t[protectedSizeMax]);

        size_t resultLen;
        Status status = _protector->protect(
            data, dataSize, protectedBuffer.get(), protectedSizeMax, &resultLen);
        if (!status.isOK()) {
            return status;
        }

        data = protectedBuffer.get();
        dataSize = resultLen;
    }

    _out->write(reinterpret_cast<const char*>(data), dataSize);
    if (_out->fail()) {
        string msg = str::stream() << "couldn't write document to file: " << _file.string()
                                   << " for remove saving: " << errnoWithDescription();
        error() << msg;
        return Status(ErrorCodes::OperationFailed, msg);
    }
    return Status::OK();
}


}  // namespace mongo
