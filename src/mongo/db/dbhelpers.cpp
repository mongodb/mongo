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


#include "mongo/db/dbhelpers.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_operation_source.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/write_ops/delete.h"
#include "mongo/db/query/write_ops/update.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

using std::string;
using std::unique_ptr;

bool Helpers::findOne(OperationContext* opCtx,
                      const CollectionAcquisition& collection,
                      const BSONObj& query,
                      BSONObj& result) {
    RecordId loc = findOne(opCtx, collection, query);
    if (loc.isNull())
        return false;
    result = collection.getCollectionPtr()->docFor(opCtx, loc).value();
    return true;
}

BSONObj Helpers::findOneForTesting(OperationContext* opCtx,
                                   const CollectionAcquisition& collection,
                                   const BSONObj& query,
                                   const bool invariantOnError) {
    BSONObj ret;
    bool found = findOne(opCtx, collection, query, ret);
    if (invariantOnError) {
        invariant(found);
    }

    return ret.getOwned();
}


/* fetch a single object from collection ns that matches query
   set your db SavedContext first
*/
RecordId Helpers::findOne(OperationContext* opCtx,
                          const CollectionAcquisition& collection,
                          const BSONObj& query) {
    if (!collection.exists())
        return RecordId();

    auto findCommand = std::make_unique<FindCommandRequest>(collection.nss());
    findCommand->setFilter(query);
    return findOne(opCtx, collection, std::move(findCommand));
}

RecordId Helpers::findOne(OperationContext* opCtx,
                          const CollectionAcquisition& collection,
                          std::unique_ptr<FindCommandRequest> findCommand) {
    if (!collection.exists())
        return RecordId();

    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{
            .findCommand = std::move(findCommand),
            .extensionsCallback = ExtensionsCallbackReal(opCtx, &collection.nss()),
            .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});
    cq->setForceGenerateRecordId(true);

    auto exec = uassertStatusOK(getExecutorFind(opCtx,
                                                MultipleCollectionAccessor{collection},
                                                std::move(cq),
                                                PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY));

    PlanExecutor::ExecState state;
    BSONObj obj;
    RecordId loc;
    if (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, &loc))) {
        return loc;
    }
    return RecordId();
}

bool Helpers::findById(OperationContext* opCtx,
                       const NamespaceString& nss,
                       BSONObj query,
                       BSONObj& result) {
    auto collCatalog = CollectionCatalog::get(opCtx);
    const Collection* collection = collCatalog->lookupCollectionByNamespace(opCtx, nss);
    if (!collection) {
        return false;
    }

    const IndexCatalog* catalog = collection->getIndexCatalog();
    const IndexDescriptor* desc = catalog->findIdIndex(opCtx);

    if (!desc) {
        if (clustered_util::isClusteredOnId(collection->getClusteredInfo())) {
            Snapshotted<BSONObj> doc;
            if (collection->findDoc(opCtx,
                                    record_id_helpers::keyForObj(IndexBoundsBuilder::objFromElement(
                                        query["_id"], collection->getDefaultCollator())),
                                    &doc)) {
                result = std::move(doc.value());
                return true;
            }
        }

        return false;
    }

    const IndexCatalogEntry* entry = catalog->getEntry(desc);
    // TODO(SERVER-103399): Investigate usage validity of CollectionPtr::CollectionPtr_UNSAFE
    auto recordId = entry->accessMethod()->asSortedData()->findSingle(
        opCtx,
        *shard_role_details::getRecoveryUnit(opCtx),
        CollectionPtr::CollectionPtr_UNSAFE(collection),
        entry,
        query["_id"].wrap());
    if (recordId.isNull())
        return false;
    result = collection->docFor(opCtx, recordId).value();
    return true;
}

RecordId Helpers::findById(OperationContext* opCtx,
                           const CollectionPtr& collection,
                           const BSONObj& idquery) {
    MONGO_verify(collection);
    const IndexCatalog* catalog = collection->getIndexCatalog();
    const IndexDescriptor* desc = catalog->findIdIndex(opCtx);
    if (!desc && clustered_util::isClusteredOnId(collection->getClusteredInfo())) {
        // There is no explicit IndexDescriptor for _id on a collection clustered by _id. However,
        // the RecordId can be constructed directly from the input.
        return record_id_helpers::keyForObj(
            IndexBoundsBuilder::objFromElement(idquery["_id"], collection->getDefaultCollator()));
    }

    uassert(13430, "no _id index", desc);
    const IndexCatalogEntry* entry = catalog->getEntry(desc);
    return entry->accessMethod()->asSortedData()->findSingle(
        opCtx,
        *shard_role_details::getRecoveryUnit(opCtx),
        collection,
        entry,
        idquery["_id"].wrap());
}

bool Helpers::getSingleton(OperationContext* opCtx, const NamespaceString& nss, BSONObj& result) {
    const auto collection = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, nss, AcquisitionPrerequisites::OperationType::kRead));
    if (!collection.exists()) {
        return false;
    }

    auto exec = InternalPlanner::collectionScan(
        opCtx, collection, PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
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

bool Helpers::getLast(OperationContext* opCtx, const NamespaceString& nss, BSONObj& result) {
#ifdef MONGO_CONFIG_DEBUG_BUILD
    boost::optional<DisableLockerRuntimeOrderingChecks> disableOrderingCheck;
    if (nss.isOplog()) {
        // This helper can violate the ordering of locks in case of index build for collections in
        // the local db. We only do that in our testing, and we do not expect users to
        // to do the same. In general, this lock ordering violation is safe because is
        // non-blocking.
        disableOrderingCheck.emplace(opCtx);
    }
#endif  // MONGO_CONFIG_DEBUG_BUILD
    const auto collection = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, nss, AcquisitionPrerequisites::OperationType::kRead));
    if (!collection.exists()) {
        return false;
    }

    auto exec = InternalPlanner::collectionScan(
        opCtx, collection, PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY, InternalPlanner::BACKWARD);
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
                             CollectionAcquisition& coll,
                             const BSONObj& o,
                             bool fromMigrate) {
    BSONElement e = o["_id"];
    MONGO_verify(stdx::to_underlying(e.type()));
    BSONObj id = e.wrap();
    return upsert(opCtx, coll, id, o, fromMigrate);
}

UpdateResult Helpers::upsert(OperationContext* opCtx,
                             CollectionAcquisition& coll,
                             const BSONObj& filter,
                             const BSONObj& updateMod,
                             bool fromMigrate) {
    AutoStatsTracker statsTracker(opCtx,
                                  coll.nss(),
                                  Top::LockType::WriteLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(coll.nss().dbName()));
    auto request = UpdateRequest();
    request.setNamespaceString(coll.nss());

    request.setQuery(filter);
    request.setUpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(updateMod));
    request.setUpsert();
    if (fromMigrate) {
        request.setSource(OperationSource::kFromMigrate);
        request.setBypassEmptyTsReplacement(true);
    }
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);

    return ::mongo::update(opCtx, coll, request);
}

void Helpers::update(OperationContext* opCtx,
                     CollectionAcquisition& coll,
                     const BSONObj& filter,
                     const BSONObj& updateMod,
                     bool fromMigrate) {
    AutoStatsTracker statsTracker(opCtx,
                                  coll.nss(),
                                  Top::LockType::WriteLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(coll.nss().dbName()));
    auto request = UpdateRequest();
    request.setNamespaceString(coll.nss());

    request.setQuery(filter);
    request.setUpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(updateMod));
    if (fromMigrate) {
        request.setSource(OperationSource::kFromMigrate);
        request.setBypassEmptyTsReplacement(true);
    }
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);

    ::mongo::update(opCtx, coll, request);
}

Status Helpers::insert(OperationContext* opCtx,
                       const CollectionAcquisition& coll,
                       const BSONObj& doc) {
    AutoStatsTracker statsTracker(opCtx,
                                  coll.nss(),
                                  Top::LockType::WriteLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(coll.nss().dbName()));
    return collection_internal::insertDocument(
        opCtx, coll.getCollectionPtr(), InsertStatement{doc}, &CurOp::get(opCtx)->debug());
}

void Helpers::deleteByRid(OperationContext* opCtx,
                          const CollectionAcquisition& coll,
                          RecordId rid) {
    AutoStatsTracker statsTracker(opCtx,
                                  coll.nss(),
                                  Top::LockType::WriteLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(coll.nss().dbName()));
    return collection_internal::deleteDocument(
        opCtx, coll.getCollectionPtr(), kUninitializedStmtId, rid, &CurOp::get(opCtx)->debug());
}

void Helpers::putSingleton(OperationContext* opCtx, CollectionAcquisition& coll, BSONObj obj) {
    AutoStatsTracker statsTracker(opCtx,
                                  coll.nss(),
                                  Top::LockType::WriteLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(coll.nss().dbName()));
    auto request = UpdateRequest();
    request.setNamespaceString(coll.nss());

    request.setUpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(obj));
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
    request.setUpsert();

    ::mongo::update(opCtx, coll, request);

    CurOp::get(opCtx)->done();
}

BSONObj Helpers::toKeyFormat(const BSONObj& o) {
    BSONObjBuilder keyObj(o.objsize());
    for (auto&& e : o) {
        keyObj.appendAs(e, "");
    }
    return keyObj.obj();
}

BSONObj Helpers::inferKeyPattern(const BSONObj& o) {
    BSONObjBuilder kpBuilder;
    for (auto&& e : o) {
        kpBuilder.append(e.fieldName(), 1);
    }
    return kpBuilder.obj();
}

void Helpers::emptyCollection(OperationContext* opCtx, const CollectionAcquisition& coll) {
    AutoStatsTracker statsTracker(opCtx,
                                  coll.nss(),
                                  Top::LockType::WriteLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(coll.nss().dbName()));
    repl::UnreplicatedWritesBlock uwb(opCtx);
    deleteObjects(opCtx, coll, BSONObj(), false);
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
    CollectionUpdateArgs args(snapshottedDoc.value());
    args.criteria = idQuery;
    args.update = BSONObj();
    collection_internal::updateDocument(opCtx,
                                        collection,
                                        recordId,
                                        snapshottedDoc,
                                        result,
                                        collection_internal::kUpdateNoIndexes,
                                        nullptr /* indexesAffected */,
                                        nullptr /* opDebug */,
                                        &args);

    return true;
}

}  // namespace mongo
