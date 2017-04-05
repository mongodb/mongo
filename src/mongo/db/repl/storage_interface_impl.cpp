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
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/collection_bulk_loader_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

const char StorageInterfaceImpl::kDefaultMinValidNamespace[] = "local.replset.minvalid";
const char StorageInterfaceImpl::kInitialSyncFlagFieldName[] = "doingInitialSync";
const char StorageInterfaceImpl::kBeginFieldName[] = "begin";
const char StorageInterfaceImpl::kOplogDeleteFromPointFieldName[] = "oplogDeleteFromPoint";

namespace {
using UniqueLock = stdx::unique_lock<stdx::mutex>;

const BSONObj kInitialSyncFlag(BSON(StorageInterfaceImpl::kInitialSyncFlagFieldName << true));
}  // namespace

StorageInterfaceImpl::StorageInterfaceImpl()
    : StorageInterfaceImpl(NamespaceString(StorageInterfaceImpl::kDefaultMinValidNamespace)) {}

StorageInterfaceImpl::StorageInterfaceImpl(const NamespaceString& minValidNss)
    : _minValidNss(minValidNss) {}

NamespaceString StorageInterfaceImpl::getMinValidNss() const {
    return _minValidNss;
}

BSONObj StorageInterfaceImpl::getMinValidDocument(OperationContext* opCtx) const {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        Lock::DBLock dblk(opCtx, _minValidNss.db(), MODE_IS);
        Lock::CollectionLock lk(opCtx->lockState(), _minValidNss.ns(), MODE_IS);
        BSONObj doc;
        bool found = Helpers::getSingleton(opCtx, _minValidNss.ns().c_str(), doc);
        invariant(found || doc.isEmpty());
        return doc;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        opCtx, "StorageInterfaceImpl::getMinValidDocument", _minValidNss.ns());

    MONGO_UNREACHABLE;
}

void StorageInterfaceImpl::updateMinValidDocument(OperationContext* opCtx,
                                                  const BSONObj& updateSpec) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        // For now this needs to be MODE_X because it sometimes creates the collection.
        Lock::DBLock dblk(opCtx, _minValidNss.db(), MODE_X);
        Helpers::putSingleton(opCtx, _minValidNss.ns().c_str(), updateSpec);
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
        opCtx, "StorageInterfaceImpl::updateMinValidDocument", _minValidNss.ns());
}

bool StorageInterfaceImpl::getInitialSyncFlag(OperationContext* opCtx) const {
    const BSONObj doc = getMinValidDocument(opCtx);
    const auto flag = doc[kInitialSyncFlagFieldName].trueValue();
    LOG(3) << "returning initial sync flag value of " << flag;
    return flag;
}

void StorageInterfaceImpl::setInitialSyncFlag(OperationContext* opCtx) {
    LOG(3) << "setting initial sync flag";
    updateMinValidDocument(opCtx, BSON("$set" << kInitialSyncFlag));
    opCtx->recoveryUnit()->waitUntilDurable();
}

void StorageInterfaceImpl::clearInitialSyncFlag(OperationContext* opCtx) {
    LOG(3) << "clearing initial sync flag";

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    OpTime time = replCoord->getMyLastAppliedOpTime();
    updateMinValidDocument(
        opCtx,
        BSON("$unset" << kInitialSyncFlag << "$set"
                      << BSON("ts" << time.getTimestamp() << "t" << time.getTerm()
                                   << kBeginFieldName
                                   << time.toBSON())));

    if (getGlobalServiceContext()->getGlobalStorageEngine()->isDurable()) {
        opCtx->recoveryUnit()->waitUntilDurable();
        replCoord->setMyLastDurableOpTime(time);
    }
}

OpTime StorageInterfaceImpl::getMinValid(OperationContext* opCtx) const {
    const BSONObj doc = getMinValidDocument(opCtx);
    const auto opTimeStatus = OpTime::parseFromOplogEntry(doc);
    // If any of the keys (fields) are missing from the minvalid document, we return
    // a null OpTime.
    if (opTimeStatus == ErrorCodes::NoSuchKey) {
        return {};
    }

    if (!opTimeStatus.isOK()) {
        severe() << "Error parsing minvalid entry: " << redact(doc)
                 << ", with status:" << opTimeStatus.getStatus();
        fassertFailedNoTrace(40052);
    }

    OpTime minValid = opTimeStatus.getValue();
    LOG(3) << "returning minvalid: " << minValid.toString() << "(" << minValid.toBSON() << ")";

    return minValid;
}

void StorageInterfaceImpl::setMinValid(OperationContext* opCtx, const OpTime& minValid) {
    LOG(3) << "setting minvalid to exactly: " << minValid.toString() << "(" << minValid.toBSON()
           << ")";
    updateMinValidDocument(
        opCtx, BSON("$set" << BSON("ts" << minValid.getTimestamp() << "t" << minValid.getTerm())));
}

void StorageInterfaceImpl::setMinValidToAtLeast(OperationContext* opCtx, const OpTime& minValid) {
    LOG(3) << "setting minvalid to at least: " << minValid.toString() << "(" << minValid.toBSON()
           << ")";
    updateMinValidDocument(
        opCtx, BSON("$max" << BSON("ts" << minValid.getTimestamp() << "t" << minValid.getTerm())));
}

void StorageInterfaceImpl::setOplogDeleteFromPoint(OperationContext* opCtx,
                                                   const Timestamp& timestamp) {
    LOG(3) << "setting oplog delete from point to: " << timestamp.toStringPretty();
    updateMinValidDocument(opCtx,
                           BSON("$set" << BSON(kOplogDeleteFromPointFieldName << timestamp)));
}

Timestamp StorageInterfaceImpl::getOplogDeleteFromPoint(OperationContext* opCtx) {
    const BSONObj doc = getMinValidDocument(opCtx);
    Timestamp out = {};
    if (auto field = doc[kOplogDeleteFromPointFieldName]) {
        out = field.timestamp();
    }

    LOG(3) << "returning oplog delete from point: " << out;
    return out;
}

void StorageInterfaceImpl::setAppliedThrough(OperationContext* opCtx, const OpTime& optime) {
    LOG(3) << "setting appliedThrough to: " << optime.toString() << "(" << optime.toBSON() << ")";
    if (optime.isNull()) {
        updateMinValidDocument(opCtx, BSON("$unset" << BSON(kBeginFieldName << 1)));
    } else {
        updateMinValidDocument(opCtx, BSON("$set" << BSON(kBeginFieldName << optime.toBSON())));
    }
}

OpTime StorageInterfaceImpl::getAppliedThrough(OperationContext* opCtx) {
    const BSONObj doc = getMinValidDocument(opCtx);
    const auto opTimeStatus = OpTime::parseFromOplogEntry(doc.getObjectField(kBeginFieldName));
    if (!opTimeStatus.isOK()) {
        // Return null OpTime on any parse failure, including if "begin" is missing.
        return {};
    }

    OpTime appliedThrough = opTimeStatus.getValue();
    LOG(3) << "returning appliedThrough: " << appliedThrough.toString() << "("
           << appliedThrough.toBSON() << ")";

    return appliedThrough;
}

StatusWith<std::unique_ptr<CollectionBulkLoader>>
StorageInterfaceImpl::createCollectionForBulkLoading(
    const NamespaceString& nss,
    const CollectionOptions& options,
    const BSONObj idIndexSpec,
    const std::vector<BSONObj>& secondaryIndexSpecs) {

    LOG(2) << "StorageInterfaceImpl::createCollectionForBulkLoading called for ns: " << nss.ns();
    auto threadPool =
        stdx::make_unique<OldThreadPool>(1, str::stream() << "InitialSyncInserters-" << nss.ns());
    std::unique_ptr<TaskRunner> runner = stdx::make_unique<TaskRunner>(threadPool.get());

    // Setup cond_var for signalling when done.
    std::unique_ptr<CollectionBulkLoader> loaderToReturn;
    Collection* collection;

    auto status = runner->runSynchronousTask([&](OperationContext* opCtx) -> Status {
        // We are not replicating nor validating writes under this OperationContext*.
        // The OperationContext* is used for all writes to the (newly) cloned collection.
        UnreplicatedWritesBlock uwb(opCtx);
        documentValidationDisabled(opCtx) = true;

        // Retry if WCE.
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            // Get locks and create the collection.
            auto db = stdx::make_unique<AutoGetOrCreateDb>(opCtx, nss.db(), MODE_IX);
            auto coll = stdx::make_unique<AutoGetCollection>(opCtx, nss, MODE_X);
            collection = coll->getCollection();

            if (collection) {
                return {ErrorCodes::NamespaceExists, "Collection already exists."};
            }

            // Create the collection.
            WriteUnitOfWork wunit(opCtx);
            collection = db->getDb()->createCollection(opCtx, nss.ns(), options, false);
            invariant(collection);
            wunit.commit();
            coll = stdx::make_unique<AutoGetCollection>(opCtx, nss, MODE_IX);

            // Move locks into loader, so it now controls their lifetime.
            auto loader = stdx::make_unique<CollectionBulkLoaderImpl>(opCtx,
                                                                      collection,
                                                                      idIndexSpec,
                                                                      std::move(threadPool),
                                                                      std::move(runner),
                                                                      std::move(db),
                                                                      std::move(coll));

            // Move the loader into the StatusWith.
            loaderToReturn = std::move(loader);
            return Status::OK();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "beginCollectionClone", nss.ns());
        MONGO_UNREACHABLE;
    });

    if (!status.isOK()) {
        return status;
    }

    invariant(collection);
    status = loaderToReturn->init(collection, secondaryIndexSpecs);
    if (!status.isOK()) {
        return status;
    }
    return std::move(loaderToReturn);
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
 * Shared implementation between findDocuments and deleteDocuments.
 */
enum class FindDeleteMode { kFind, kDelete };
StatusWith<std::vector<BSONObj>> _findOrDeleteDocuments(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<StringData> indexName,
    StorageInterface::ScanDirection scanDirection,
    const BSONObj& startKey,
    BoundInclusion boundInclusion,
    std::size_t limit,
    FindDeleteMode mode) {
    auto isFind = mode == FindDeleteMode::kFind;
    auto opStr = isFind ? "StorageInterfaceImpl::findOne" : "StorageInterfaceImpl::deleteOne";

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
                                  boundInclusion,
                                  limit,
                                  FindDeleteMode::kDelete);
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
