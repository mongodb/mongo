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
#include "mongo/db/namespace_string.h"
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

/* fetch a single object from collection ns that matches query
   set your db SavedContext first
*/
bool Helpers::findOne(OperationContext* opCtx,
                      Collection* collection,
                      const BSONObj& query,
                      BSONObj& result,
                      bool requireIndex) {
    RecordId loc = findOne(opCtx, collection, query, requireIndex);
    if (loc.isNull())
        return false;
    result = collection->docFor(opCtx, loc).value();
    return true;
}

/* fetch a single object from collection ns that matches query
   set your db SavedContext first
*/
RecordId Helpers::findOne(OperationContext* opCtx,
                          Collection* collection,
                          const BSONObj& query,
                          bool requireIndex) {
    if (!collection)
        return RecordId();

    const ExtensionsCallbackReal extensionsCallback(opCtx, &collection->ns());

    auto qr = stdx::make_unique<QueryRequest>(collection->ns());
    qr->setFilter(query);

    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, std::move(qr), extensionsCallback);
    massert(17244, "Could not canonicalize " + query.toString(), statusWithCQ.isOK());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    size_t options = requireIndex ? QueryPlannerParams::NO_TABLE_SCAN : QueryPlannerParams::DEFAULT;
    auto statusWithPlanExecutor =
        getExecutor(opCtx, collection, std::move(cq), PlanExecutor::NO_YIELD, options);
    massert(17245,
            "Could not get executor for query " + query.toString(),
            statusWithPlanExecutor.isOK());

    auto exec = std::move(statusWithPlanExecutor.getValue());
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

bool Helpers::findById(OperationContext* opCtx,
                       Database* database,
                       StringData ns,
                       BSONObj query,
                       BSONObj& result,
                       bool* nsFound,
                       bool* indexFound) {
    invariant(database);

    Collection* collection = database->getCollection(opCtx, ns);
    if (!collection) {
        return false;
    }

    if (nsFound)
        *nsFound = true;

    IndexCatalog* catalog = collection->getIndexCatalog();
    const IndexDescriptor* desc = catalog->findIdIndex(opCtx);

    if (!desc)
        return false;

    if (indexFound)
        *indexFound = 1;

    RecordId loc = catalog->getIndex(desc)->findSingle(opCtx, query["_id"].wrap());
    if (loc.isNull())
        return false;
    result = collection->docFor(opCtx, loc).value();
    return true;
}

RecordId Helpers::findById(OperationContext* opCtx,
                           Collection* collection,
                           const BSONObj& idquery) {
    verify(collection);
    IndexCatalog* catalog = collection->getIndexCatalog();
    const IndexDescriptor* desc = catalog->findIdIndex(opCtx);
    uassert(13430, "no _id index", desc);
    return catalog->getIndex(desc)->findSingle(opCtx, idquery["_id"].wrap());
}

bool Helpers::getSingleton(OperationContext* opCtx, const char* ns, BSONObj& result) {
    AutoGetCollectionForReadCommand ctx(opCtx, NamespaceString(ns));
    auto exec =
        InternalPlanner::collectionScan(opCtx, ns, ctx.getCollection(), PlanExecutor::NO_YIELD);
    PlanExecutor::ExecState state = exec->getNext(&result, NULL);

    CurOp::get(opCtx)->done();

    // Non-yielding collection scans from InternalPlanner will never error.
    invariant(PlanExecutor::ADVANCED == state || PlanExecutor::IS_EOF == state);

    if (PlanExecutor::ADVANCED == state) {
        result = result.getOwned();
        return true;
    }

    return false;
}

bool Helpers::getLast(OperationContext* opCtx, const char* ns, BSONObj& result) {
    AutoGetCollectionForReadCommand autoColl(opCtx, NamespaceString(ns));
    auto exec = InternalPlanner::collectionScan(
        opCtx, ns, autoColl.getCollection(), PlanExecutor::NO_YIELD, InternalPlanner::BACKWARD);
    PlanExecutor::ExecState state = exec->getNext(&result, NULL);

    // Non-yielding collection scans from InternalPlanner will never error.
    invariant(PlanExecutor::ADVANCED == state || PlanExecutor::IS_EOF == state);

    if (PlanExecutor::ADVANCED == state) {
        result = result.getOwned();
        return true;
    }

    return false;
}

void Helpers::upsert(OperationContext* opCtx,
                     const string& ns,
                     const BSONObj& o,
                     bool fromMigrate) {
    BSONElement e = o["_id"];
    verify(e.type());
    BSONObj id = e.wrap();

    OldClientContext context(opCtx, ns);

    const NamespaceString requestNs(ns);
    UpdateRequest request(requestNs);

    request.setQuery(id);
    request.setUpdates(o);
    request.setUpsert();
    request.setFromMigration(fromMigrate);
    UpdateLifecycleImpl updateLifecycle(requestNs);
    request.setLifecycle(&updateLifecycle);

    update(opCtx, context.db(), request);
}

void Helpers::putSingleton(OperationContext* opCtx, const char* ns, BSONObj obj) {
    OldClientContext context(opCtx, ns);

    const NamespaceString requestNs(ns);
    UpdateRequest request(requestNs);

    request.setUpdates(obj);
    request.setUpsert();
    UpdateLifecycleImpl updateLifecycle(requestNs);
    request.setLifecycle(&updateLifecycle);

    update(opCtx, context.db(), request);

    CurOp::get(opCtx)->done();
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

long long Helpers::removeRange(OperationContext* opCtx,
                               const KeyRange& range,
                               BoundInclusion boundInclusion,
                               const WriteConcernOptions& writeConcern,
                               RemoveSaver* callback,
                               bool fromMigrate,
                               bool onlyRemoveOrphanedDocs) {
    Timer rangeRemoveTimer;
    const NamespaceString nss(range.ns);

    // The IndexChunk has a keyPattern that may apply to more than one index - we need to
    // select the index and get the full index keyPattern here.
    std::string indexName;
    BSONObj min;
    BSONObj max;

    {
        AutoGetCollectionForReadCommand ctx(opCtx, nss);
        Collection* collection = ctx.getCollection();
        if (!collection) {
            warning(LogComponent::kSharding)
                << "collection deleted before cleaning data over range of type " << range.keyPattern
                << " in " << nss.ns() << endl;
            return -1;
        }

        // Allow multiKey based on the invariant that shard keys must be single-valued.
        // Therefore, any multi-key index prefixed by shard key cannot be multikey over
        // the shard key fields.
        const IndexDescriptor* idx =
            collection->getIndexCatalog()->findShardKeyPrefixedIndex(opCtx,
                                                                     range.keyPattern,
                                                                     false);  // requireSingleKey
        if (!idx) {
            warning(LogComponent::kSharding) << "no index found to clean data over range of type "
                                             << range.keyPattern << " in " << nss.ns() << endl;
            return -1;
        }

        indexName = idx->indexName();
        KeyPattern indexKeyPattern(idx->keyPattern());

        // Extend bounds to match the index we found

        invariant(IndexBounds::isStartIncludedInBound(boundInclusion));
        // Extend min to get (min, MinKey, MinKey, ....)
        min = Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(range.minKey, false));
        // If upper bound is included, extend max to get (max, MaxKey, MaxKey, ...)
        // If not included, extend max to get (max, MinKey, MinKey, ....)
        const bool maxInclusive = IndexBounds::isEndIncludedInBound(boundInclusion);
        max = Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(range.maxKey, maxInclusive));
    }


    MONGO_LOG_COMPONENT(1, LogComponent::kSharding)
        << "begin removal of " << min << " to " << max << " in " << nss.ns()
        << " with write concern: " << writeConcern.toBSON() << endl;

    long long numDeleted = 0;

    Milliseconds millisWaitingForReplication{0};

    while (1) {
        // Scoping for write lock.
        {
            AutoGetCollection ctx(opCtx, nss, MODE_IX, MODE_IX);
            Collection* collection = ctx.getCollection();
            if (!collection)
                break;

            IndexDescriptor* desc =
                collection->getIndexCatalog()->findIndexByName(opCtx, indexName);

            if (!desc) {
                warning(LogComponent::kSharding) << "shard key index '" << indexName << "' on '"
                                                 << nss.ns() << "' was dropped";
                return -1;
            }

            auto exec = InternalPlanner::indexScan(opCtx,
                                                   collection,
                                                   desc,
                                                   min,
                                                   max,
                                                   boundInclusion,
                                                   PlanExecutor::YIELD_AUTO,
                                                   InternalPlanner::FORWARD,
                                                   InternalPlanner::IXSCAN_FETCH);

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
                    << min << " to " << max << " in " << nss.ns() << ": "
                    << WorkingSetCommon::toStatusString(obj)
                    << ", stats: " << Explain::getWinningPlanStats(exec.get()) << endl;
                break;
            }

            verify(PlanExecutor::ADVANCED == state);

            WriteUnitOfWork wuow(opCtx);

            if (onlyRemoveOrphanedDocs) {
                // Do a final check in the write lock to make absolutely sure that our
                // collection hasn't been modified in a way that invalidates our migration
                // cleanup.

                // We should never be able to turn off the sharding state once enabled, but
                // in the future we might want to.
                verify(ShardingState::get(opCtx)->enabled());

                bool docIsOrphan;

                // In write lock, so will be the most up-to-date version
                auto metadataNow = CollectionShardingState::get(opCtx, nss.ns())->getMetadata();
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
                        << ", collection " << nss.ns() << " has changed " << endl;
                    break;
                }
            }

            if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, nss)) {
                warning() << "stepped down from primary while deleting chunk; "
                          << "orphaning data in " << nss.ns() << " in range [" << redact(min)
                          << ", " << redact(max) << ")";
                return numDeleted;
            }

            if (callback)
                callback->goingToDelete(obj);

            OpDebug* const nullOpDebug = nullptr;
            collection->deleteDocument(opCtx, rloc, nullOpDebug, fromMigrate);
            wuow.commit();
            numDeleted++;
        }

        // TODO remove once the yielding below that references this timer has been removed
        Timer secondaryThrottleTime;

        if (writeConcern.shouldWaitForOtherNodes() && numDeleted > 0) {
            repl::ReplicationCoordinator::StatusAndDuration replStatus =
                repl::getGlobalReplicationCoordinator()->awaitReplication(
                    opCtx,
                    repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
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
                                                    << " in " << nss.ns() << " (took "
                                                    << rangeRemoveTimer.millis() << "ms)" << endl;

    return numDeleted;
}

void Helpers::emptyCollection(OperationContext* opCtx, const NamespaceString& nss) {
    OldClientContext context(opCtx, nss.ns());
    repl::UnreplicatedWritesBlock uwb(opCtx);
    Collection* collection = context.db() ? context.db()->getCollection(opCtx, nss) : nullptr;
    deleteObjects(opCtx, collection, nss, BSONObj(), false);
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
                     << redact(status);
            fassertFailed(34350);
        }

        _out->write(reinterpret_cast<const char*>(protectedBuffer.get()), resultLen);
        if (_out->fail()) {
            severe() << "Couldn't write finalized DataProtector data to: " << _file.string()
                     << " for remove saving: " << redact(errnoWithDescription());
            fassertFailed(34351);
        }

        protectedBuffer.reset(new uint8_t[protectedSizeMax]);
        status = _protector->finalizeTag(protectedBuffer.get(), protectedSizeMax, &resultLen);
        if (!status.isOK()) {
            severe() << "Unable to get finalizeTag from DataProtector while closing RemoveSaver: "
                     << redact(status);
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
                     << " for remove saving: " << redact(errnoWithDescription());
            fassertFailed(34354);
        }
    }
}

Status Helpers::RemoveSaver::goingToDelete(const BSONObj& o) {
    if (!_out) {
        // We don't expect to ever pass "" to create_directories below, but catch
        // this anyway as per SERVER-26412.
        invariant(!_root.empty());
        boost::filesystem::create_directories(_root);
        _out.reset(new ofstream(_file.string().c_str(), ios_base::out | ios_base::binary));
        if (_out->fail()) {
            string msg = str::stream() << "couldn't create file: " << _file.string()
                                       << " for remove saving: " << redact(errnoWithDescription());
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
                                   << " for remove saving: " << redact(errnoWithDescription());
        error() << msg;
        return Status(ErrorCodes::OperationFailed, msg);
    }
    return Status::OK();
}


}  // namespace mongo
