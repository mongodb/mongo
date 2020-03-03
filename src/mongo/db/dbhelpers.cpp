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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

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
#include "mongo/db/ops/update_result.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
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

    auto qr = std::make_unique<QueryRequest>(collection->ns());
    qr->setFilter(query);
    return findOne(opCtx, collection, std::move(qr), requireIndex);
}

RecordId Helpers::findOne(OperationContext* opCtx,
                          Collection* collection,
                          std::unique_ptr<QueryRequest> qr,
                          bool requireIndex) {
    if (!collection)
        return RecordId();

    const ExtensionsCallbackReal extensionsCallback(opCtx, &collection->ns());

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(qr),
                                     expCtx,
                                     extensionsCallback,
                                     MatchExpressionParser::kAllowAllSpecialFeatures);

    massertStatusOK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    size_t options = requireIndex ? QueryPlannerParams::NO_TABLE_SCAN : QueryPlannerParams::DEFAULT;
    auto exec = uassertStatusOK(
        getExecutor(opCtx, collection, std::move(cq), PlanExecutor::NO_YIELD, options));

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

    Collection* collection =
        CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, NamespaceString(ns));
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

    RecordId loc = catalog->getEntry(desc)->accessMethod()->findSingle(opCtx, query["_id"].wrap());
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
    return catalog->getEntry(desc)->accessMethod()->findSingle(opCtx, idquery["_id"].wrap());
}

// Acquires necessary locks to read the collection with the given namespace. If this is an oplog
// read, use AutoGetOplog for simplified locking.
Collection* getCollectionForRead(OperationContext* opCtx,
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
    auto collection = getCollectionForRead(opCtx, NamespaceString(ns), autoColl, autoOplog);

    auto exec = InternalPlanner::collectionScan(opCtx, ns, collection, PlanExecutor::NO_YIELD);
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
    auto collection = getCollectionForRead(opCtx, NamespaceString(ns), autoColl, autoOplog);

    auto exec = InternalPlanner::collectionScan(
        opCtx, ns, collection, PlanExecutor::NO_YIELD, InternalPlanner::BACKWARD);
    PlanExecutor::ExecState state = exec->getNext(&result, nullptr);

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
    upsert(opCtx, ns, id, o, fromMigrate);
}

void Helpers::upsert(OperationContext* opCtx,
                     const string& ns,
                     const BSONObj& filter,
                     const BSONObj& updateMod,
                     bool fromMigrate) {
    OldClientContext context(opCtx, ns);

    const NamespaceString requestNs(ns);
    UpdateRequest request(requestNs);

    request.setQuery(filter);
    request.setUpdateModification(updateMod);
    request.setUpsert();
    request.setFromMigration(fromMigrate);
    request.setYieldPolicy(PlanExecutor::NO_YIELD);

    update(opCtx, context.db(), request);
}

void Helpers::putSingleton(OperationContext* opCtx, const char* ns, BSONObj obj) {
    OldClientContext context(opCtx, ns);

    const NamespaceString requestNs(ns);
    UpdateRequest request(requestNs);

    request.setUpdateModification(obj);
    request.setUpsert();

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

void Helpers::emptyCollection(OperationContext* opCtx, const NamespaceString& nss) {
    OldClientContext context(opCtx, nss.ns());
    repl::UnreplicatedWritesBlock uwb(opCtx);
    Collection* collection = context.db()
        ? CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss)
        : nullptr;
    deleteObjects(opCtx, collection, nss, BSONObj(), false);
}

}  // namespace mongo
