/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/dbhelpers.h"

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
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {

using std::set;
using std::string;
using std::unique_ptr;

/* fetch a single object from collection ns that matches query
   set your db SavedContext first
*/
bool Helpers::findOne(OperationContext* opCtx,
                      const CollectionPtr& collection,
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
                          const CollectionPtr& collection,
                          const BSONObj& query,
                          bool requireIndex) {
    if (!collection)
        return RecordId();

    auto findCommand = std::make_unique<FindCommandRequest>(collection->ns());
    findCommand->setFilter(query);
    return findOne(opCtx, collection, std::move(findCommand), requireIndex);
}

RecordId Helpers::findOne(OperationContext* opCtx,
                          const CollectionPtr& collection,
                          std::unique_ptr<FindCommandRequest> findCommand,
                          bool requireIndex) {
    if (!collection)
        return RecordId();

    const ExtensionsCallbackReal extensionsCallback(opCtx, &collection->ns());

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(findCommand),
                                     false,
                                     expCtx,
                                     extensionsCallback,
                                     MatchExpressionParser::kAllowAllSpecialFeatures);

    massertStatusOK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    size_t options = requireIndex ? QueryPlannerParams::NO_TABLE_SCAN : QueryPlannerParams::DEFAULT;
    auto exec = uassertStatusOK(getExecutor(
        opCtx, &collection, std::move(cq), PlanYieldPolicy::YieldPolicy::NO_YIELD, options));

    PlanExecutor::ExecState state;
    BSONObj obj;
    RecordId loc;
    if (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, &loc))) {
        return loc;
    }
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

    // TODO ForRead?
    NamespaceString nss{ns};
    CollectionPtr collection =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    if (!collection) {
        return false;
    }

    if (nsFound)
        *nsFound = true;

    const IndexCatalog* catalog = collection->getIndexCatalog();
    const IndexDescriptor* desc = catalog->findIdIndex(opCtx);

    bool isTimeseriesBucketNs = nss.isTimeseriesBucketsCollection();
    if (!desc && !isTimeseriesBucketNs) {
        return false;
    }

    if (indexFound)
        *indexFound = 1;

    // A time-series buckets collection does not have an index on _id. However, the RecordId can be
    // constructed using the _id field. So we can retrive the document by using the RecordId
    // instead.
    if (isTimeseriesBucketNs) {
        Snapshotted<BSONObj> doc;
        if (collection->findDoc(
                opCtx, RecordId(record_id_helpers::keyForOID(query["_id"].OID())), &doc)) {
            result = std::move(doc.value());
            return true;
        }
        return false;
    }

    RecordId loc = catalog->getEntry(desc)->accessMethod()->findSingle(opCtx, query["_id"].wrap());
    if (loc.isNull())
        return false;
    result = collection->docFor(opCtx, loc).value();
    return true;
}

RecordId Helpers::findById(OperationContext* opCtx,
                           const CollectionPtr& collection,
                           const BSONObj& idquery) {
    verify(collection);
    const IndexCatalog* catalog = collection->getIndexCatalog();
    const IndexDescriptor* desc = catalog->findIdIndex(opCtx);
    uassert(13430, "no _id index", desc);
    return catalog->getEntry(desc)->accessMethod()->findSingle(opCtx, idquery["_id"].wrap());
}

// Acquires necessary locks to read the collection with the given namespace. If this is an oplog
// read, use AutoGetOplog for simplified locking.
const CollectionPtr& getCollectionForRead(
    OperationContext* opCtx,
    const NamespaceString& ns,
    boost::optional<AutoGetCollectionForReadCommand>& autoColl,
    boost::optional<AutoGetOplog>& autoOplog) {
    if (ns.isOplog()) {
        // Simplify locking rules for oplog collection.
        autoOplog.emplace(opCtx, OplogAccessMode::kRead);
        return autoOplog->getCollection();
    } else {
        autoColl.emplace(opCtx, NamespaceString(ns));
        return autoColl->getCollection();
    }
}

bool Helpers::getSingleton(OperationContext* opCtx, const char* ns, BSONObj& result) {
    boost::optional<AutoGetCollectionForReadCommand> autoColl;
    boost::optional<AutoGetOplog> autoOplog;
    const auto& collection = getCollectionForRead(opCtx, NamespaceString(ns), autoColl, autoOplog);
    if (!collection) {
        return false;
    }

    auto exec =
        InternalPlanner::collectionScan(opCtx, &collection, PlanYieldPolicy::YieldPolicy::NO_YIELD);
    PlanExecutor::ExecState state = exec->getNext(&result, nullptr);

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
    boost::optional<AutoGetCollectionForReadCommand> autoColl;
    boost::optional<AutoGetOplog> autoOplog;
    const auto& collection = getCollectionForRead(opCtx, NamespaceString(ns), autoColl, autoOplog);
    if (!collection) {
        return false;
    }

    auto exec = InternalPlanner::collectionScan(
        opCtx, &collection, PlanYieldPolicy::YieldPolicy::NO_YIELD, InternalPlanner::BACKWARD);
    PlanExecutor::ExecState state = exec->getNext(&result, nullptr);

    // Non-yielding collection scans from InternalPlanner will never error.
    invariant(PlanExecutor::ADVANCED == state || PlanExecutor::IS_EOF == state);

    if (PlanExecutor::ADVANCED == state) {
        result = result.getOwned();
        return true;
    }

    return false;
}

UpdateResult Helpers::upsert(OperationContext* opCtx,
                             const string& ns,
                             const BSONObj& o,
                             bool fromMigrate) {
    BSONElement e = o["_id"];
    verify(e.type());
    BSONObj id = e.wrap();
    return upsert(opCtx, ns, id, o, fromMigrate);
}

UpdateResult Helpers::upsert(OperationContext* opCtx,
                             const string& ns,
                             const BSONObj& filter,
                             const BSONObj& updateMod,
                             bool fromMigrate) {
    OldClientContext context(opCtx, ns);

    const NamespaceString requestNs(ns);
    auto request = UpdateRequest();
    request.setNamespaceString(requestNs);

    request.setQuery(filter);
    request.setUpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(updateMod));
    request.setUpsert();
    if (fromMigrate) {
        request.setSource(OperationSource::kFromMigrate);
    }
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::NO_YIELD);

    return ::mongo::update(opCtx, context.db(), request);
}

void Helpers::update(OperationContext* opCtx,
                     const string& ns,
                     const BSONObj& filter,
                     const BSONObj& updateMod,
                     bool fromMigrate) {
    OldClientContext context(opCtx, ns);

    const NamespaceString requestNs(ns);
    auto request = UpdateRequest();
    request.setNamespaceString(requestNs);

    request.setQuery(filter);
    request.setUpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(updateMod));
    if (fromMigrate) {
        request.setSource(OperationSource::kFromMigrate);
    }
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::NO_YIELD);

    ::mongo::update(opCtx, context.db(), request);
}

void Helpers::putSingleton(OperationContext* opCtx, const char* ns, BSONObj obj) {
    OldClientContext context(opCtx, ns);

    const NamespaceString requestNs(ns);
    auto request = UpdateRequest();
    request.setNamespaceString(requestNs);

    request.setUpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(obj));
    request.setUpsert();

    ::mongo::update(opCtx, context.db(), request);

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

void Helpers::emptyCollection(OperationContext* opCtx, const NamespaceString& nss) {
    OldClientContext context(opCtx, nss.ns());
    repl::UnreplicatedWritesBlock uwb(opCtx);
    CollectionPtr collection = context.db()
        ? CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)
        : nullptr;
    deleteObjects(opCtx, collection, nss, BSONObj(), false);
}

bool Helpers::findByIdAndNoopUpdate(OperationContext* opCtx,
                                    const CollectionPtr& collection,
                                    const BSONObj& idQuery,
                                    BSONObj& result) {
    auto recordId = Helpers::findById(opCtx, collection, idQuery);
    if (recordId.isNull()) {
        return false;
    }

    Snapshotted<BSONObj> snapshottedDoc;
    auto foundDoc = collection->findDoc(opCtx, recordId, &snapshottedDoc);
    if (!foundDoc) {
        return false;
    }

    result = snapshottedDoc.value();

    // Use an UnreplicatedWritesBlock to avoid generating an oplog entry for this no-op update.
    // The update is being used to generated write conflicts and isn't modifying the data itself, so
    // secondaries don't need to know about it. Also set CollectionUpdateArgs::update to an empty
    // BSONObj because that's a second way OpObserverImpl::onUpdate() detects and ignores no-op
    // updates.
    repl::UnreplicatedWritesBlock uwb(opCtx);
    CollectionUpdateArgs args;
    args.criteria = idQuery;
    args.update = BSONObj();
    collection->updateDocument(opCtx, recordId, snapshottedDoc, result, false, nullptr, &args);

    return true;
}

}  // namespace mongo
