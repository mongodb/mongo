/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/repl/storage_interface_impl.h"

#include <algorithm>
#include <boost/optional.hpp>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/collection_bulk_loader_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/rollback_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

const char StorageInterfaceImpl::kDefaultRollbackIdNamespace[] = "local.system.rollback.id";
const char StorageInterfaceImpl::kRollbackIdFieldName[] = "rollbackId";
const char StorageInterfaceImpl::kRollbackIdDocumentId[] = "rollbackId";

namespace {
using UniqueLock = stdx::unique_lock<stdx::mutex>;

const auto kIdIndexName = "_id_"_sd;

}  // namespace

StorageInterfaceImpl::StorageInterfaceImpl()
    : _rollbackIdNss(StorageInterfaceImpl::kDefaultRollbackIdNamespace) {}

StatusWith<int> StorageInterfaceImpl::getRollbackID(OperationContext* opCtx) {
    BSONObjBuilder bob;
    bob.append("_id", kRollbackIdDocumentId);
    auto id = bob.obj();

    try {
        auto rbidDoc = findById(opCtx, _rollbackIdNss, id["_id"]);
        if (!rbidDoc.isOK()) {
            return rbidDoc.getStatus();
        }

        auto rbid = RollbackID::parse(IDLParserErrorContext("RollbackID"), rbidDoc.getValue());
        invariant(rbid.get_id() == kRollbackIdDocumentId);
        return rbid.getRollbackId();
    } catch (...) {
        return exceptionToStatus();
    }

    MONGO_UNREACHABLE;
}

Status StorageInterfaceImpl::initializeRollbackID(OperationContext* opCtx) {
    auto status = createCollection(opCtx, _rollbackIdNss, CollectionOptions());
    if (!status.isOK()) {
        return status;
    }

    RollbackID rbid;
    rbid.set_id(kRollbackIdDocumentId);
    rbid.setRollbackId(0);

    BSONObjBuilder bob;
    rbid.serialize(&bob);
    return insertDocument(opCtx, _rollbackIdNss, bob.done());
}

Status StorageInterfaceImpl::incrementRollbackID(OperationContext* opCtx) {
    // This is safe because this is only called during rollback, and you can not have two
    // rollbacks at once.
    auto rbid = getRollbackID(opCtx);
    if (!rbid.isOK()) {
        return rbid.getStatus();
    }

    // If we would go over the integer limit, reset the Rollback ID to 0.
    BSONObjBuilder updateBob;
    if (rbid.getValue() == std::numeric_limits<int>::max()) {
        BSONObjBuilder setBob(updateBob.subobjStart("$set"));
        setBob.append(kRollbackIdFieldName, 0);
    } else {
        BSONObjBuilder incBob(updateBob.subobjStart("$inc"));
        incBob.append(kRollbackIdFieldName, 1);
    }

    // Since the Rollback ID is in a singleton collection, we can fix the _id field.
    BSONObjBuilder bob;
    bob.append("_id", kRollbackIdDocumentId);
    auto id = bob.obj();
    Status status = upsertById(opCtx, _rollbackIdNss, id["_id"], updateBob.obj());

    // We wait until durable so that we are sure the Rollback ID is updated before rollback ends.
    if (status.isOK()) {
        opCtx->recoveryUnit()->waitUntilDurable();
    }
    return status;
}

StatusWith<std::unique_ptr<CollectionBulkLoader>>
StorageInterfaceImpl::createCollectionForBulkLoading(
    const NamespaceString& nss,
    const CollectionOptions& options,
    const BSONObj idIndexSpec,
    const std::vector<BSONObj>& secondaryIndexSpecs) {

    LOG(2) << "StorageInterfaceImpl::createCollectionForBulkLoading called for ns: " << nss.ns();

    class StashClient {
    public:
        StashClient() {
            if (Client::getCurrent()) {
                _stashedClient = Client::releaseCurrent();
            }
        }
        ~StashClient() {
            if (Client::getCurrent()) {
                Client::releaseCurrent();
            }
            if (_stashedClient) {
                Client::setCurrent(std::move(_stashedClient));
            }
        }

    private:
        ServiceContext::UniqueClient _stashedClient;
    } stash;
    Client::setCurrent(
        getGlobalServiceContext()->makeClient(str::stream() << nss.ns() << " loader"));
    auto opCtx = cc().makeOperationContext();

    documentValidationDisabled(opCtx.get()) = true;

    std::unique_ptr<AutoGetCollection> autoColl;
    // Retry if WCE.
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        UnreplicatedWritesBlock uwb(opCtx.get());

        // Get locks and create the collection.
        AutoGetOrCreateDb db(opCtx.get(), nss.db(), MODE_X);
        AutoGetCollection coll(opCtx.get(), nss, MODE_IX);

        if (coll.getCollection()) {
            return {ErrorCodes::NamespaceExists,
                    str::stream() << "Collection " << nss.ns() << " already exists."};
        }
        {
            // Create the collection.
            WriteUnitOfWork wunit(opCtx.get());
            fassert(40332, db.getDb()->createCollection(opCtx.get(), nss.ns(), options, false));
            wunit.commit();
        }

        autoColl = stdx::make_unique<AutoGetCollection>(opCtx.get(), nss, MODE_IX);

        // Build empty capped indexes.  Capped indexes cannot be built by the MultiIndexBlock
        // because the cap might delete documents off the back while we are inserting them into
        // the front.
        if (options.capped) {
            WriteUnitOfWork wunit(opCtx.get());
            if (!idIndexSpec.isEmpty()) {
                auto status =
                    autoColl->getCollection()->getIndexCatalog()->createIndexOnEmptyCollection(
                        opCtx.get(), idIndexSpec);
                if (!status.getStatus().isOK()) {
                    return status.getStatus();
                }
            }
            for (auto&& spec : secondaryIndexSpecs) {
                auto status =
                    autoColl->getCollection()->getIndexCatalog()->createIndexOnEmptyCollection(
                        opCtx.get(), spec);
                if (!status.getStatus().isOK()) {
                    return status.getStatus();
                }
            }
            wunit.commit();
        }
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx.get(), "beginCollectionClone", nss.ns());

    // Move locks into loader, so it now controls their lifetime.
    auto loader =
        stdx::make_unique<CollectionBulkLoaderImpl>(Client::releaseCurrent(),
                                                    std::move(opCtx),
                                                    std::move(autoColl),
                                                    options.capped ? BSONObj() : idIndexSpec);

    auto status = loader->init(options.capped ? std::vector<BSONObj>() : secondaryIndexSpecs);
    if (!status.isOK()) {
        return status;
    }
    return {std::move(loader)};
}

Status StorageInterfaceImpl::insertDocument(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const BSONObj& doc) {
    return insertDocuments(opCtx, nss, {doc});
}

namespace {

/**
 * Returns Collection* from database RAII object.
 * Returns NamespaceNotFound if the database or collection does not exist.
 */
template <typename AutoGetCollectionType>
StatusWith<Collection*> getCollection(const AutoGetCollectionType& autoGetCollection,
                                      const NamespaceString& nss,
                                      const std::string& message) {
    if (!autoGetCollection.getDb()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Database [" << nss.db() << "] not found. " << message};
    }

    auto collection = autoGetCollection.getCollection();
    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection [" << nss.ns() << "] not found. " << message};
    }

    return collection;
}

Status insertDocumentsSingleBatch(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  std::vector<BSONObj>::const_iterator begin,
                                  std::vector<BSONObj>::const_iterator end) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);

    auto collectionResult =
        getCollection(autoColl, nss, "The collection must exist before inserting documents.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    auto collection = collectionResult.getValue();

    WriteUnitOfWork wunit(opCtx);
    OpDebug* const nullOpDebug = nullptr;
    auto status = collection->insertDocuments(opCtx, begin, end, nullOpDebug, false);
    if (!status.isOK()) {
        return status;
    }
    wunit.commit();

    return Status::OK();
}

}  // namespace

Status StorageInterfaceImpl::insertDocuments(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const std::vector<BSONObj>& docs) {
    if (docs.size() > 1U) {
        try {
            if (insertDocumentsSingleBatch(opCtx, nss, docs.cbegin(), docs.cend()).isOK()) {
                return Status::OK();
            }
        } catch (...) {
            // Ignore this failure and behave as-if we never tried to do the combined batch insert.
            // The loop below will handle reporting any non-transient errors.
        }
    }

    // Try to insert the batch one-at-a-time because the batch failed all-at-once inserting.
    for (auto it = docs.cbegin(); it != docs.cend(); ++it) {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            auto status = insertDocumentsSingleBatch(opCtx, nss, it, it + 1);
            if (!status.isOK()) {
                return status;
            }
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
            opCtx, "StorageInterfaceImpl::insertDocuments", nss.ns());
    }

    return Status::OK();
}

Status StorageInterfaceImpl::dropReplicatedDatabases(OperationContext* opCtx) {
    dropAllDatabasesExceptLocal(opCtx);
    return Status::OK();
}

Status StorageInterfaceImpl::createOplog(OperationContext* opCtx, const NamespaceString& nss) {
    mongo::repl::createOplog(opCtx, nss.ns(), true);
    return Status::OK();
}

StatusWith<size_t> StorageInterfaceImpl::getOplogMaxSize(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    auto collectionResult = getCollection(autoColl, nss, "Your oplog doesn't exist.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    auto collection = collectionResult.getValue();

    const auto options = collection->getCatalogEntry()->getCollectionOptions(opCtx);
    if (!options.capped)
        return {ErrorCodes::BadValue, str::stream() << nss.ns() << " isn't capped"};

    return options.cappedSize;
}

Status StorageInterfaceImpl::createCollection(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const CollectionOptions& options) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        AutoGetOrCreateDb databaseWriteGuard(opCtx, nss.db(), MODE_X);
        auto db = databaseWriteGuard.getDb();
        invariant(db);
        if (db->getCollection(opCtx, nss)) {
            return {ErrorCodes::NamespaceExists,
                    str::stream() << "Collection " << nss.ns() << " already exists."};
        }
        WriteUnitOfWork wuow(opCtx);
        try {
            auto coll = db->createCollection(opCtx, nss.ns(), options);
            invariant(coll);
        } catch (const UserException& ex) {
            return ex.toStatus();
        }
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "StorageInterfaceImpl::createCollection", nss.ns());
    return Status::OK();
}

Status StorageInterfaceImpl::dropCollection(OperationContext* opCtx, const NamespaceString& nss) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        AutoGetDb autoDB(opCtx, nss.db(), MODE_X);
        if (!autoDB.getDb()) {
            // Database does not exist - nothing to do.
            return Status::OK();
        }
        WriteUnitOfWork wunit(opCtx);
        const auto status = autoDB.getDb()->dropCollectionEvenIfSystem(opCtx, nss);
        if (status.isOK()) {
            wunit.commit();
        }
        return status;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "StorageInterfaceImpl::dropCollection", nss.ns());
}

Status StorageInterfaceImpl::renameCollection(OperationContext* opCtx,
                                              const NamespaceString& fromNS,
                                              const NamespaceString& toNS,
                                              bool stayTemp) {
    if (fromNS.db() != toNS.db()) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Cannot rename collection between databases. From NS: "
                                    << fromNS.ns()
                                    << "; to NS: "
                                    << toNS.ns());
    }

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        AutoGetDb autoDB(opCtx, fromNS.db(), MODE_X);
        if (!autoDB.getDb()) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "Cannot rename collection from " << fromNS.ns() << " to "
                                        << toNS.ns()
                                        << ". Database "
                                        << fromNS.db()
                                        << " not found.");
        }
        WriteUnitOfWork wunit(opCtx);
        const auto status =
            autoDB.getDb()->renameCollection(opCtx, fromNS.ns(), toNS.ns(), stayTemp);
        if (status.isOK()) {
            wunit.commit();
        }
        return status;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        opCtx, "StorageInterfaceImpl::renameCollection", fromNS.ns());
}

namespace {

/**
 * Returns DeleteStageParams for deleteOne with fetch.
 */
DeleteStageParams makeDeleteStageParamsForDeleteDocuments() {
    DeleteStageParams deleteStageParams;
    deleteStageParams.isMulti = true;
    deleteStageParams.returnDeleted = true;
    return deleteStageParams;
}

/**
 * Shared implementation between findDocuments, deleteDocuments, and _findOrDeleteById.
 * _findOrDeleteById is used by findById, and deleteById.
 */
enum class FindDeleteMode { kFind, kDelete };
StatusWith<std::vector<BSONObj>> _findOrDeleteDocuments(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<StringData> indexName,
    StorageInterface::ScanDirection scanDirection,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    std::size_t limit,
    FindDeleteMode mode) {
    auto isFind = mode == FindDeleteMode::kFind;
    auto opStr = isFind ? "StorageInterfaceImpl::find" : "StorageInterfaceImpl::delete";

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        auto collectionAccessMode = isFind ? MODE_IS : MODE_IX;
        AutoGetCollection autoColl(opCtx, nss, collectionAccessMode);
        auto collectionResult = getCollection(
            autoColl, nss, str::stream() << "Unable to proceed with " << opStr << ".");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        auto collection = collectionResult.getValue();

        auto isForward = scanDirection == StorageInterface::ScanDirection::kForward;
        auto direction = isForward ? InternalPlanner::FORWARD : InternalPlanner::BACKWARD;

        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> planExecutor;
        if (!indexName) {
            if (!startKey.isEmpty()) {
                return {ErrorCodes::NoSuchKey,
                        "non-empty startKey not allowed for collection scan"};
            }
            if (boundInclusion != BoundInclusion::kIncludeStartKeyOnly) {
                return {ErrorCodes::InvalidOptions,
                        "bound inclusion must be BoundInclusion::kIncludeStartKeyOnly for "
                        "collection scan"};
            }
            // Use collection scan.
            planExecutor = isFind
                ? InternalPlanner::collectionScan(
                      opCtx, nss.ns(), collection, PlanExecutor::NO_YIELD, direction)
                : InternalPlanner::deleteWithCollectionScan(
                      opCtx,
                      collection,
                      makeDeleteStageParamsForDeleteDocuments(),
                      PlanExecutor::NO_YIELD,
                      direction);
        } else {
            // Use index scan.
            auto indexCatalog = collection->getIndexCatalog();
            invariant(indexCatalog);
            bool includeUnfinishedIndexes = false;
            IndexDescriptor* indexDescriptor =
                indexCatalog->findIndexByName(opCtx, *indexName, includeUnfinishedIndexes);
            if (!indexDescriptor) {
                return {ErrorCodes::IndexNotFound,
                        str::stream() << "Index not found, ns:" << nss.ns() << ", index: "
                                      << *indexName};
            }
            if (indexDescriptor->isPartial()) {
                return {ErrorCodes::IndexOptionsConflict,
                        str::stream() << "Partial index is not allowed for this operation, ns:"
                                      << nss.ns()
                                      << ", index: "
                                      << *indexName};
            }

            KeyPattern keyPattern(indexDescriptor->keyPattern());
            auto minKey = Helpers::toKeyFormat(keyPattern.extendRangeBound({}, false));
            auto maxKey = Helpers::toKeyFormat(keyPattern.extendRangeBound({}, true));
            auto bounds =
                isForward ? std::make_pair(minKey, maxKey) : std::make_pair(maxKey, minKey);
            if (!startKey.isEmpty()) {
                bounds.first = startKey;
            }
            if (!endKey.isEmpty()) {
                bounds.second = endKey;
            }
            planExecutor = isFind
                ? InternalPlanner::indexScan(opCtx,
                                             collection,
                                             indexDescriptor,
                                             bounds.first,
                                             bounds.second,
                                             boundInclusion,
                                             PlanExecutor::NO_YIELD,
                                             direction,
                                             InternalPlanner::IXSCAN_FETCH)
                : InternalPlanner::deleteWithIndexScan(opCtx,
                                                       collection,
                                                       makeDeleteStageParamsForDeleteDocuments(),
                                                       indexDescriptor,
                                                       bounds.first,
                                                       bounds.second,
                                                       boundInclusion,
                                                       PlanExecutor::NO_YIELD,
                                                       direction);
        }

        std::vector<BSONObj> docs;
        while (docs.size() < limit) {
            BSONObj doc;
            auto state = planExecutor->getNext(&doc, nullptr);
            if (PlanExecutor::ADVANCED == state) {
                docs.push_back(doc.getOwned());
            } else {
                invariant(PlanExecutor::IS_EOF == state);
                break;
            }
        }
        return docs;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, opStr, nss.ns());
    MONGO_UNREACHABLE;
}

StatusWith<BSONObj> _findOrDeleteById(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const BSONElement& idKey,
                                      FindDeleteMode mode) {
    auto wrappedIdKey = idKey.wrap("");
    auto result = _findOrDeleteDocuments(opCtx,
                                         nss,
                                         kIdIndexName,
                                         StorageInterface::ScanDirection::kForward,
                                         wrappedIdKey,
                                         wrappedIdKey,
                                         BoundInclusion::kIncludeBothStartAndEndKeys,
                                         1U,
                                         mode);
    if (!result.isOK()) {
        return result.getStatus();
    }
    const auto& docs = result.getValue();
    if (docs.empty()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "No document found with _id: " << idKey};
    }

    return docs.front();
}

}  // namespace

StatusWith<std::vector<BSONObj>> StorageInterfaceImpl::findDocuments(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<StringData> indexName,
    ScanDirection scanDirection,
    const BSONObj& startKey,
    BoundInclusion boundInclusion,
    std::size_t limit) {
    return _findOrDeleteDocuments(opCtx,
                                  nss,
                                  indexName,
                                  scanDirection,
                                  startKey,
                                  {},
                                  boundInclusion,
                                  limit,
                                  FindDeleteMode::kFind);
}

StatusWith<std::vector<BSONObj>> StorageInterfaceImpl::deleteDocuments(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<StringData> indexName,
    ScanDirection scanDirection,
    const BSONObj& startKey,
    BoundInclusion boundInclusion,
    std::size_t limit) {
    return _findOrDeleteDocuments(opCtx,
                                  nss,
                                  indexName,
                                  scanDirection,
                                  startKey,
                                  {},
                                  boundInclusion,
                                  limit,
                                  FindDeleteMode::kDelete);
}

StatusWith<BSONObj> StorageInterfaceImpl::findSingleton(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    auto result = findDocuments(opCtx,
                                nss,
                                boost::none,  // Collection scan.
                                StorageInterface::ScanDirection::kForward,
                                {},  // Start at the beginning of the collection.
                                BoundInclusion::kIncludeStartKeyOnly,
                                2U);  // Ask for 2 documents to ensure it's a singleton.
    if (!result.isOK()) {
        return result.getStatus();
    }

    const auto& docs = result.getValue();
    if (docs.empty()) {
        return {ErrorCodes::CollectionIsEmpty,
                str::stream() << "No document found in namespace: " << nss.ns()};
    } else if (docs.size() != 1U) {
        return {ErrorCodes::TooManyMatchingDocuments,
                str::stream() << "More than singleton document found in namespace: " << nss.ns()};
    }

    return docs.front();
}

StatusWith<BSONObj> StorageInterfaceImpl::findById(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const BSONElement& idKey) {
    return _findOrDeleteById(opCtx, nss, idKey, FindDeleteMode::kFind);
}

StatusWith<BSONObj> StorageInterfaceImpl::deleteById(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     const BSONElement& idKey) {
    return _findOrDeleteById(opCtx, nss, idKey, FindDeleteMode::kDelete);
}

namespace {

/**
 * Checks _id key passed to upsertById and returns a query document for UpdateRequest.
 */
StatusWith<BSONObj> makeUpsertQuery(const BSONElement& idKey) {
    auto query = BSON("_id" << idKey);

    // With the ID hack, only simple _id queries are allowed. Otherwise, UpdateStage will fail with
    // a fatal assertion.
    if (!CanonicalQuery::isSimpleIdQuery(query)) {
        return {ErrorCodes::InvalidIdField,
                str::stream() << "Unable to update document with a non-simple _id query: "
                              << query};
    }

    return query;
}

Status _upsertWithQuery(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& query,
                        const BSONObj& update) {
    UpdateRequest request(nss);
    request.setQuery(query);
    request.setUpdates(update);
    request.setUpsert(true);
    invariant(!request.isMulti());  // We only want to update one document for performance.
    invariant(!request.shouldReturnAnyDocs());
    invariant(PlanExecutor::NO_YIELD == request.getYieldPolicy());

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        // ParsedUpdate needs to be inside the write conflict retry loop because it may create a
        // CanonicalQuery whose ownership will be transferred to the plan executor in
        // getExecutorUpdate().
        ParsedUpdate parsedUpdate(opCtx, &request);
        auto parsedUpdateStatus = parsedUpdate.parseRequest();
        if (!parsedUpdateStatus.isOK()) {
            return parsedUpdateStatus;
        }

        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto collectionResult = getCollection(
            autoColl,
            nss,
            str::stream() << "Unable to update documents in " << nss.ns() << " using query "
                          << query);
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        auto collection = collectionResult.getValue();

        auto planExecutorResult =
            mongo::getExecutorUpdate(opCtx, nullptr, collection, &parsedUpdate);
        if (!planExecutorResult.isOK()) {
            return planExecutorResult.getStatus();
        }
        auto planExecutor = std::move(planExecutorResult.getValue());

        return planExecutor->executePlan();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "_upsertWithQuery", nss.ns());

    MONGO_UNREACHABLE;
}

}  // namespace

Status StorageInterfaceImpl::upsertById(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const BSONElement& idKey,
                                        const BSONObj& update) {
    // Validate and construct an _id query for UpdateResult.
    // The _id key will be passed directly to IDHackStage.
    auto queryResult = makeUpsertQuery(idKey);
    if (!queryResult.isOK()) {
        return queryResult.getStatus();
    }
    auto query = queryResult.getValue();

    UpdateRequest request(nss);
    request.setQuery(query);
    request.setUpdates(update);
    request.setUpsert(true);
    invariant(!request.isMulti());  // This follows from using an exact _id query.
    invariant(!request.shouldReturnAnyDocs());
    invariant(PlanExecutor::NO_YIELD == request.getYieldPolicy());

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        // ParsedUpdate needs to be inside the write conflict retry loop because it contains
        // the UpdateDriver whose state may be modified while we are applying the update.
        ParsedUpdate parsedUpdate(opCtx, &request);
        auto parsedUpdateStatus = parsedUpdate.parseRequest();
        if (!parsedUpdateStatus.isOK()) {
            return parsedUpdateStatus;
        }

        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto collectionResult = getCollection(autoColl, nss, "Unable to update document.");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        auto collection = collectionResult.getValue();

        // We're using the ID hack to perform the update so we have to disallow collections
        // without an _id index.
        auto descriptor = collection->getIndexCatalog()->findIdIndex(opCtx);
        if (!descriptor) {
            return {ErrorCodes::IndexNotFound,
                    "Unable to update document in a collection without an _id index."};
        }

        UpdateStageParams updateStageParams(
            parsedUpdate.getRequest(), parsedUpdate.getDriver(), nullptr);
        auto planExecutor = InternalPlanner::updateWithIdHack(opCtx,
                                                              collection,
                                                              updateStageParams,
                                                              descriptor,
                                                              idKey.wrap(""),
                                                              parsedUpdate.yieldPolicy());

        return planExecutor->executePlan();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "StorageInterfaceImpl::upsertById", nss.ns());

    MONGO_UNREACHABLE;
}

Status StorageInterfaceImpl::putSingleton(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& update) {
    return _upsertWithQuery(opCtx, nss, {}, update);
}

Status StorageInterfaceImpl::deleteByFilter(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const BSONObj& filter) {
    DeleteRequest request(nss);
    request.setQuery(filter);
    request.setMulti(true);
    request.setYieldPolicy(PlanExecutor::NO_YIELD);

    // This disables the legalClientSystemNS() check in getExecutorDelete() which is used to
    // disallow client deletes from unrecognized system collections.
    request.setGod();

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        // ParsedDelete needs to be inside the write conflict retry loop because it may create a
        // CanonicalQuery whose ownership will be transferred to the plan executor in
        // getExecutorDelete().
        ParsedDelete parsedDelete(opCtx, &request);
        auto parsedDeleteStatus = parsedDelete.parseRequest();
        if (!parsedDeleteStatus.isOK()) {
            return parsedDeleteStatus;
        }

        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto collectionResult = getCollection(
            autoColl,
            nss,
            str::stream() << "Unable to delete documents in " << nss.ns() << " using filter "
                          << filter);
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        auto collection = collectionResult.getValue();

        auto planExecutorResult =
            mongo::getExecutorDelete(opCtx, nullptr, collection, &parsedDelete);
        if (!planExecutorResult.isOK()) {
            return planExecutorResult.getStatus();
        }
        auto planExecutor = std::move(planExecutorResult.getValue());

        return planExecutor->executePlan();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "StorageInterfaceImpl::deleteByFilter", nss.ns());

    MONGO_UNREACHABLE;
}

StatusWith<StorageInterface::CollectionSize> StorageInterfaceImpl::getCollectionSize(
    OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForRead autoColl(opCtx, nss);

    auto collectionResult =
        getCollection(autoColl, nss, "Unable to get total size of documents in collection.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    auto collection = collectionResult.getValue();

    return collection->dataSize(opCtx);
}

StatusWith<StorageInterface::CollectionCount> StorageInterfaceImpl::getCollectionCount(
    OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForRead autoColl(opCtx, nss);

    auto collectionResult =
        getCollection(autoColl, nss, "Unable to get number of documents in collection.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    auto collection = collectionResult.getValue();

    return collection->numRecords(opCtx);
}

Status StorageInterfaceImpl::isAdminDbValid(OperationContext* opCtx) {
    AutoGetDb autoDB(opCtx, "admin", MODE_X);
    auto adminDb = autoDB.getDb();
    if (!adminDb) {
        return Status::OK();
    }

    Collection* const usersCollection =
        adminDb->getCollection(opCtx, AuthorizationManager::usersCollectionNamespace);
    const bool hasUsers =
        usersCollection && !Helpers::findOne(opCtx, usersCollection, BSONObj(), false).isNull();
    Collection* const adminVersionCollection =
        adminDb->getCollection(opCtx, AuthorizationManager::versionCollectionNamespace);
    BSONObj authSchemaVersionDocument;
    if (!adminVersionCollection ||
        !Helpers::findOne(opCtx,
                          adminVersionCollection,
                          AuthorizationManager::versionDocumentQuery,
                          authSchemaVersionDocument)) {
        if (!hasUsers) {
            // It's OK to have no auth version document if there are no user documents.
            return Status::OK();
        }
        std::string msg = str::stream()
            << "During initial sync, found documents in "
            << AuthorizationManager::usersCollectionNamespace.ns()
            << " but could not find an auth schema version document in "
            << AuthorizationManager::versionCollectionNamespace.ns() << ".  "
            << "This indicates that the primary of this replica set was not successfully "
               "upgraded to schema version "
            << AuthorizationManager::schemaVersion26Final
            << ", which is the minimum supported schema version in this version of MongoDB";
        return {ErrorCodes::AuthSchemaIncompatible, msg};
    }
    long long foundSchemaVersion;
    Status status = bsonExtractIntegerField(authSchemaVersionDocument,
                                            AuthorizationManager::schemaVersionFieldName,
                                            &foundSchemaVersion);
    if (!status.isOK()) {
        std::string msg = str::stream()
            << "During initial sync, found malformed auth schema version document: "
            << status.toString() << "; document: " << authSchemaVersionDocument;
        return {ErrorCodes::AuthSchemaIncompatible, msg};
    }
    if ((foundSchemaVersion != AuthorizationManager::schemaVersion26Final) &&
        (foundSchemaVersion != AuthorizationManager::schemaVersion28SCRAM)) {
        std::string msg = str::stream()
            << "During initial sync, found auth schema version " << foundSchemaVersion
            << ", but this version of MongoDB only supports schema versions "
            << AuthorizationManager::schemaVersion26Final << " and "
            << AuthorizationManager::schemaVersion28SCRAM;
        return {ErrorCodes::AuthSchemaIncompatible, msg};
    }

    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
