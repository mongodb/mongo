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
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/collection_bulk_loader_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
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

StatusWith<int> StorageInterfaceImpl::initializeRollbackID(OperationContext* opCtx) {
    auto status = createCollection(opCtx, _rollbackIdNss, CollectionOptions());
    if (!status.isOK()) {
        return status;
    }

    RollbackID rbid;
    int initRBID = 1;
    rbid.set_id(kRollbackIdDocumentId);
    rbid.setRollbackId(initRBID);

    BSONObjBuilder bob;
    rbid.serialize(&bob);
    Timestamp noTimestamp;  // This write is not replicated.
    status = insertDocument(opCtx,
                            _rollbackIdNss,
                            TimestampedBSONObj{bob.done(), noTimestamp},
                            OpTime::kUninitializedTerm);
    if (status.isOK()) {
        return initRBID;
    } else {
        return status;
    }
}

StatusWith<int> StorageInterfaceImpl::incrementRollbackID(OperationContext* opCtx) {
    // This is safe because this is only called during rollback, and you can not have two
    // rollbacks at once.
    auto rbidSW = getRollbackID(opCtx);
    if (!rbidSW.isOK()) {
        return rbidSW;
    }

    // If we would go over the integer limit, reset the Rollback ID to 1.
    BSONObjBuilder updateBob;
    int newRBID = -1;
    if (rbidSW.getValue() == std::numeric_limits<int>::max()) {
        newRBID = 1;
        BSONObjBuilder setBob(updateBob.subobjStart("$set"));
        setBob.append(kRollbackIdFieldName, newRBID);
    } else {
        BSONObjBuilder incBob(updateBob.subobjStart("$inc"));
        incBob.append(kRollbackIdFieldName, 1);
        newRBID = rbidSW.getValue() + 1;
    }

    // Since the Rollback ID is in a singleton collection, we can fix the _id field.
    BSONObjBuilder bob;
    bob.append("_id", kRollbackIdDocumentId);
    auto id = bob.obj();
    Status status = upsertById(opCtx, _rollbackIdNss, id["_id"], updateBob.obj());

    // We wait until durable so that we are sure the Rollback ID is updated before rollback ends.
    if (status.isOK()) {
        opCtx->recoveryUnit()->waitUntilDurable();
        return newRBID;
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
    Status status = writeConflictRetry(opCtx.get(), "beginCollectionClone", nss.ns(), [&] {
        UnreplicatedWritesBlock uwb(opCtx.get());

        // Get locks and create the collection.
        AutoGetOrCreateDb db(opCtx.get(), nss.db(), MODE_X);
        AutoGetCollection coll(opCtx.get(), nss, MODE_IX);

        if (coll.getCollection()) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "Collection " << nss.ns() << " already exists.");
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

        return Status::OK();
    });

    if (!status.isOK()) {
        return status;
    }

    // Move locks into loader, so it now controls their lifetime.
    auto loader =
        stdx::make_unique<CollectionBulkLoaderImpl>(Client::releaseCurrent(),
                                                    std::move(opCtx),
                                                    std::move(autoColl),
                                                    options.capped ? BSONObj() : idIndexSpec);

    status = loader->init(options.capped ? std::vector<BSONObj>() : secondaryIndexSpecs);
    if (!status.isOK()) {
        return status;
    }
    return {std::move(loader)};
}

Status StorageInterfaceImpl::insertDocument(OperationContext* opCtx,
                                            const NamespaceStringOrUUID& nsOrUUID,
                                            const TimestampedBSONObj& doc,
                                            long long term) {
    return insertDocuments(opCtx, nsOrUUID, {InsertStatement(doc.obj, doc.timestamp, term)});
}

namespace {

/**
 * Returns Collection* from database RAII object.
 * Returns NamespaceNotFound if the database or collection does not exist.
 */
template <typename AutoGetCollectionType>
StatusWith<Collection*> getCollection(const AutoGetCollectionType& autoGetCollection,
                                      const NamespaceStringOrUUID& nsOrUUID,
                                      const std::string& message) {
    if (!autoGetCollection.getDb()) {
        StringData dbName = nsOrUUID.nss() ? nsOrUUID.nss()->db() : nsOrUUID.dbname();
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Database [" << dbName << "] not found. " << message};
    }

    auto collection = autoGetCollection.getCollection();
    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection [" << nsOrUUID.toString() << "] not found. "
                              << message};
    }

    return collection;
}

Status insertDocumentsSingleBatch(OperationContext* opCtx,
                                  const NamespaceStringOrUUID& nsOrUUID,
                                  std::vector<InsertStatement>::const_iterator begin,
                                  std::vector<InsertStatement>::const_iterator end) {
    AutoGetCollection autoColl(opCtx, nsOrUUID, MODE_IX);

    auto collectionResult =
        getCollection(autoColl, nsOrUUID, "The collection must exist before inserting documents.");
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
                                             const NamespaceStringOrUUID& nsOrUUID,
                                             const std::vector<InsertStatement>& docs) {
    if (docs.size() > 1U) {
        try {
            if (insertDocumentsSingleBatch(opCtx, nsOrUUID, docs.cbegin(), docs.cend()).isOK()) {
                return Status::OK();
            }
        } catch (...) {
            // Ignore this failure and behave as-if we never tried to do the combined batch insert.
            // The loop below will handle reporting any non-transient errors.
        }
    }

    // Try to insert the batch one-at-a-time because the batch failed all-at-once inserting.
    for (auto it = docs.cbegin(); it != docs.cend(); ++it) {
        auto status = writeConflictRetry(
            opCtx, "StorageInterfaceImpl::insertDocuments", nsOrUUID.toString(), [&] {
                auto status = insertDocumentsSingleBatch(opCtx, nsOrUUID, it, it + 1);
                if (!status.isOK()) {
                    return status;
                }

                return Status::OK();
            });

        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status StorageInterfaceImpl::dropReplicatedDatabases(OperationContext* opCtx) {
    Database::dropAllDatabasesExceptLocal(opCtx);
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
    return writeConflictRetry(opCtx, "StorageInterfaceImpl::createCollection", nss.ns(), [&] {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetOrCreateDb databaseWriteGuard(opCtx, nss.db(), MODE_X);
        auto db = databaseWriteGuard.getDb();
        invariant(db);
        if (db->getCollection(opCtx, nss)) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "Collection " << nss.ns() << " already exists.");
        }
        WriteUnitOfWork wuow(opCtx);
        try {
            auto coll = db->createCollection(opCtx, nss.ns(), options);
            invariant(coll);
        } catch (const AssertionException& ex) {
            return ex.toStatus();
        }
        wuow.commit();

        return Status::OK();
    });
}

Status StorageInterfaceImpl::dropCollection(OperationContext* opCtx, const NamespaceString& nss) {
    return writeConflictRetry(opCtx, "StorageInterfaceImpl::dropCollection", nss.ns(), [&] {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetDb autoDB(opCtx, nss.db(), MODE_X);
        if (!autoDB.getDb()) {
            // Database does not exist - nothing to do.
            return Status::OK();
        }
        WriteUnitOfWork wunit(opCtx);
        const auto status = autoDB.getDb()->dropCollectionEvenIfSystem(opCtx, nss);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();
        return Status::OK();
    });
}

Status StorageInterfaceImpl::truncateCollection(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    return writeConflictRetry(opCtx, "StorageInterfaceImpl::truncateCollection", nss.ns(), [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        auto collectionResult =
            getCollection(autoColl, nss, "The collection must exist before truncating.");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        auto collection = collectionResult.getValue();

        WriteUnitOfWork wunit(opCtx);
        const auto status = collection->truncate(opCtx);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();
        return Status::OK();
    });
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

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::renameCollection", fromNS.ns(), [&] {
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
        if (!status.isOK()) {
            return status;
        }

        auto newColl = autoDB.getDb()->getCollection(opCtx, toNS);
        if (newColl->uuid()) {
            UUIDCatalog::get(opCtx).onRenameCollection(opCtx, newColl, newColl->uuid().get());
        }
        wunit.commit();
        return status;
    });
}

Status StorageInterfaceImpl::setIndexIsMultikey(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const std::string& indexName,
                                                const MultikeyPaths& paths,
                                                Timestamp ts) {
    if (ts.isNull()) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "Cannot set index " << indexName << " on " << nss.ns()
                                    << " as multikey at null timestamp");
    }

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::setIndexIsMultikey", nss.ns(), [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        auto collectionResult = getCollection(
            autoColl, nss, "The collection must exist before setting an index to multikey.");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        auto collection = collectionResult.getValue();

        WriteUnitOfWork wunit(opCtx);
        auto tsResult = opCtx->recoveryUnit()->setTimestamp(ts);
        if (!tsResult.isOK()) {
            return tsResult;
        }

        auto idx = collection->getIndexCatalog()->findIndexByName(
            opCtx, indexName, true /* includeUnfinishedIndexes */);
        if (!idx) {
            return Status(ErrorCodes::IndexNotFound,
                          str::stream() << "Could not find index " << indexName << " in "
                                        << nss.ns()
                                        << " to set to multikey.");
        }
        collection->getIndexCatalog()->getIndex(idx)->setIndexIsMultikey(opCtx, paths);
        wunit.commit();
        return Status::OK();
    });
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
    const NamespaceStringOrUUID& nsOrUUID,
    boost::optional<StringData> indexName,
    StorageInterface::ScanDirection scanDirection,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    std::size_t limit,
    FindDeleteMode mode) {
    auto isFind = mode == FindDeleteMode::kFind;
    auto opStr = isFind ? "StorageInterfaceImpl::find" : "StorageInterfaceImpl::delete";

    return writeConflictRetry(
        opCtx, opStr, nsOrUUID.toString(), [&]() -> StatusWith<std::vector<BSONObj>> {
            // We need to explicitly use this in a few places to help the type inference.  Use a
            // shorthand.
            using Result = StatusWith<std::vector<BSONObj>>;

            auto collectionAccessMode = isFind ? MODE_IS : MODE_IX;
            AutoGetCollection autoColl(opCtx, nsOrUUID, collectionAccessMode);
            auto collectionResult = getCollection(
                autoColl, nsOrUUID, str::stream() << "Unable to proceed with " << opStr << ".");
            if (!collectionResult.isOK()) {
                return Result(collectionResult.getStatus());
            }
            auto collection = collectionResult.getValue();

            auto isForward = scanDirection == StorageInterface::ScanDirection::kForward;
            auto direction = isForward ? InternalPlanner::FORWARD : InternalPlanner::BACKWARD;

            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> planExecutor;
            if (!indexName) {
                if (!startKey.isEmpty()) {
                    return Result(ErrorCodes::NoSuchKey,
                                  "non-empty startKey not allowed for collection scan");
                }
                if (boundInclusion != BoundInclusion::kIncludeStartKeyOnly) {
                    return Result(
                        ErrorCodes::InvalidOptions,
                        "bound inclusion must be BoundInclusion::kIncludeStartKeyOnly for "
                        "collection scan");
                }
                // Use collection scan.
                planExecutor = isFind
                    ? InternalPlanner::collectionScan(
                          opCtx, nsOrUUID.toString(), collection, PlanExecutor::NO_YIELD, direction)
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
                    return Result(ErrorCodes::IndexNotFound,
                                  str::stream() << "Index not found, ns:" << nsOrUUID.toString()
                                                << ", index: "
                                                << *indexName);
                }
                if (indexDescriptor->isPartial()) {
                    return Result(ErrorCodes::IndexOptionsConflict,
                                  str::stream()
                                      << "Partial index is not allowed for this operation, ns:"
                                      << nsOrUUID.toString()
                                      << ", index: "
                                      << *indexName);
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
                planExecutor = isFind ? InternalPlanner::indexScan(opCtx,
                                                                   collection,
                                                                   indexDescriptor,
                                                                   bounds.first,
                                                                   bounds.second,
                                                                   boundInclusion,
                                                                   PlanExecutor::NO_YIELD,
                                                                   direction,
                                                                   InternalPlanner::IXSCAN_FETCH)
                                      : InternalPlanner::deleteWithIndexScan(
                                            opCtx,
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
            BSONObj out;
            PlanExecutor::ExecState state = PlanExecutor::ExecState::ADVANCED;
            while (state == PlanExecutor::ExecState::ADVANCED && docs.size() < limit) {
                state = planExecutor->getNext(&out, nullptr);
                if (state == PlanExecutor::ExecState::ADVANCED) {
                    docs.push_back(out.getOwned());
                }
            }

            switch (state) {
                case PlanExecutor::ADVANCED:
                case PlanExecutor::IS_EOF:
                    return Result(docs);
                case PlanExecutor::FAILURE:
                case PlanExecutor::DEAD:
                    return WorkingSetCommon::getMemberObjectStatus(out);
                default:
                    MONGO_UNREACHABLE;
            }
        });
}

StatusWith<BSONObj> _findOrDeleteById(OperationContext* opCtx,
                                      const NamespaceStringOrUUID& nsOrUUID,
                                      const BSONElement& idKey,
                                      FindDeleteMode mode) {
    auto wrappedIdKey = idKey.wrap("");
    auto result = _findOrDeleteDocuments(opCtx,
                                         nsOrUUID,
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
        return {ErrorCodes::NoSuchKey,
                str::stream() << "No document found with _id: " << redact(idKey) << " in namespace "
                              << nsOrUUID.toString()};
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
                                                   const NamespaceStringOrUUID& nsOrUUID,
                                                   const BSONElement& idKey) {
    return _findOrDeleteById(opCtx, nsOrUUID, idKey, FindDeleteMode::kFind);
}

StatusWith<BSONObj> StorageInterfaceImpl::deleteById(OperationContext* opCtx,
                                                     const NamespaceStringOrUUID& nsOrUUID,
                                                     const BSONElement& idKey) {
    return _findOrDeleteById(opCtx, nsOrUUID, idKey, FindDeleteMode::kDelete);
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

Status _updateWithQuery(OperationContext* opCtx,
                        const UpdateRequest& request,
                        const Timestamp& ts) {
    invariant(!request.isMulti());  // We only want to update one document for performance.
    invariant(!request.shouldReturnAnyDocs());
    invariant(PlanExecutor::NO_YIELD == request.getYieldPolicy());

    auto& nss = request.getNamespaceString();
    return writeConflictRetry(opCtx, "_updateWithQuery", nss.ns(), [&] {
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
                          << request.getQuery());
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        auto collection = collectionResult.getValue();
        WriteUnitOfWork wuow(opCtx);
        if (!ts.isNull()) {
            uassertStatusOK(opCtx->recoveryUnit()->setTimestamp(ts));
        }

        auto planExecutorResult =
            mongo::getExecutorUpdate(opCtx, nullptr, collection, &parsedUpdate);
        if (!planExecutorResult.isOK()) {
            return planExecutorResult.getStatus();
        }
        auto planExecutor = std::move(planExecutorResult.getValue());

        auto ret = planExecutor->executePlan();
        wuow.commit();
        return ret;
    });
}

}  // namespace

Status StorageInterfaceImpl::upsertById(OperationContext* opCtx,
                                        const NamespaceStringOrUUID& nsOrUUID,
                                        const BSONElement& idKey,
                                        const BSONObj& update) {
    // Validate and construct an _id query for UpdateResult.
    // The _id key will be passed directly to IDHackStage.
    auto queryResult = makeUpsertQuery(idKey);
    if (!queryResult.isOK()) {
        return queryResult.getStatus();
    }
    auto query = queryResult.getValue();

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::upsertById", nsOrUUID.toString(), [&] {
        AutoGetCollection autoColl(opCtx, nsOrUUID, MODE_IX);
        auto collectionResult = getCollection(autoColl, nsOrUUID, "Unable to update document.");
        if (!collectionResult.isOK()) {
            return collectionResult.getStatus();
        }
        auto collection = collectionResult.getValue();

        // We can create an UpdateRequest now that the collection's namespace has been resolved, in
        // the event it was specified as a UUID.
        UpdateRequest request(collection->ns());
        request.setQuery(query);
        request.setUpdates(update);
        request.setUpsert(true);
        invariant(!request.isMulti());  // This follows from using an exact _id query.
        invariant(!request.shouldReturnAnyDocs());
        invariant(PlanExecutor::NO_YIELD == request.getYieldPolicy());

        // ParsedUpdate needs to be inside the write conflict retry loop because it contains
        // the UpdateDriver whose state may be modified while we are applying the update.
        ParsedUpdate parsedUpdate(opCtx, &request);
        auto parsedUpdateStatus = parsedUpdate.parseRequest();
        if (!parsedUpdateStatus.isOK()) {
            return parsedUpdateStatus;
        }

        // We're using the ID hack to perform the update so we have to disallow collections
        // without an _id index.
        auto descriptor = collection->getIndexCatalog()->findIdIndex(opCtx);
        if (!descriptor) {
            return Status(ErrorCodes::IndexNotFound,
                          "Unable to update document in a collection without an _id index.");
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
    });
}

Status StorageInterfaceImpl::putSingleton(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const TimestampedBSONObj& update) {
    UpdateRequest request(nss);
    request.setQuery({});
    request.setUpdates(update.obj);
    request.setUpsert(true);
    return _updateWithQuery(opCtx, request, update.timestamp);
}

Status StorageInterfaceImpl::updateSingleton(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const BSONObj& query,
                                             const TimestampedBSONObj& update) {
    UpdateRequest request(nss);
    request.setQuery(query);
    request.setUpdates(update.obj);
    invariant(!request.isUpsert());
    return _updateWithQuery(opCtx, request, update.timestamp);
}

Status StorageInterfaceImpl::deleteByFilter(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const BSONObj& filter) {
    DeleteRequest request(nss);
    request.setQuery(filter);
    request.setMulti(true);
    request.setYieldPolicy(PlanExecutor::NO_YIELD);

    // This disables the isLegalClientSystemNS() check in getExecutorDelete() which is used to
    // disallow client deletes from unrecognized system collections.
    request.setGod();

    return writeConflictRetry(opCtx, "StorageInterfaceImpl::deleteByFilter", nss.ns(), [&] {
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
    });
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
    OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) {
    AutoGetCollectionForRead autoColl(opCtx, nsOrUUID);

    auto collectionResult =
        getCollection(autoColl, nsOrUUID, "Unable to get number of documents in collection.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    auto collection = collectionResult.getValue();

    return collection->numRecords(opCtx);
}

Status StorageInterfaceImpl::setCollectionCount(OperationContext* opCtx,
                                                const NamespaceStringOrUUID& nsOrUUID,
                                                long long newCount) {
    AutoGetCollection autoColl(opCtx, nsOrUUID, LockMode::MODE_X);

    auto collectionResult =
        getCollection(autoColl, nsOrUUID, "Unable to set number of documents in collection.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    auto collection = collectionResult.getValue();

    auto rs = collection->getRecordStore();
    // We cannot fix the data size correctly, so we just get the current cached value and keep it
    // the same.
    long long dataSize = rs->dataSize(opCtx);
    rs->updateStatsAfterRepair(opCtx, newCount, dataSize);
    return Status::OK();
}

StatusWith<OptionalCollectionUUID> StorageInterfaceImpl::getCollectionUUID(
    OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForRead autoColl(opCtx, nss);

    auto collectionResult = getCollection(
        autoColl, nss, str::stream() << "Unable to get UUID of " << nss.ns() << " collection.");
    if (!collectionResult.isOK()) {
        return collectionResult.getStatus();
    }
    auto collection = collectionResult.getValue();
    return collection->uuid();
}

Status StorageInterfaceImpl::upgradeNonReplicatedUniqueIndexes(OperationContext* opCtx) {
    return updateNonReplicatedUniqueIndexes(opCtx);
}

void StorageInterfaceImpl::setStableTimestamp(ServiceContext* serviceCtx, Timestamp snapshotName) {
    serviceCtx->getStorageEngine()->setStableTimestamp(snapshotName);
}

void StorageInterfaceImpl::setInitialDataTimestamp(ServiceContext* serviceCtx,
                                                   Timestamp snapshotName) {
    serviceCtx->getStorageEngine()->setInitialDataTimestamp(snapshotName);
}

StatusWith<Timestamp> StorageInterfaceImpl::recoverToStableTimestamp(OperationContext* opCtx) {
    return opCtx->getServiceContext()->getStorageEngine()->recoverToStableTimestamp(opCtx);
}

bool StorageInterfaceImpl::supportsRecoverToStableTimestamp(ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->supportsRecoverToStableTimestamp();
}

boost::optional<Timestamp> StorageInterfaceImpl::getRecoveryTimestamp(
    ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->getRecoveryTimestamp();
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

void StorageInterfaceImpl::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) {
    AutoGetCollection oplog(opCtx, NamespaceString::kRsOplogNamespace, MODE_IS);
    oplog.getCollection()->getRecordStore()->waitForAllEarlierOplogWritesToBeVisible(opCtx);
}

void StorageInterfaceImpl::oplogDiskLocRegister(OperationContext* opCtx,
                                                const Timestamp& ts,
                                                bool orderedCommit) {
    AutoGetCollection oplog(opCtx, NamespaceString::kRsOplogNamespace, MODE_IS);
    fassert(
        28557,
        oplog.getCollection()->getRecordStore()->oplogDiskLocRegister(opCtx, ts, orderedCommit));
}

boost::optional<Timestamp> StorageInterfaceImpl::getLastStableCheckpointTimestamp(
    ServiceContext* serviceCtx) const {
    if (!supportsRecoverToStableTimestamp(serviceCtx)) {
        return boost::none;
    }

    const auto ret = serviceCtx->getStorageEngine()->getLastStableCheckpointTimestamp();
    if (ret == boost::none) {
        return Timestamp::min();
    }

    return ret;
}

bool StorageInterfaceImpl::supportsDocLocking(ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->supportsDocLocking();
}

Timestamp StorageInterfaceImpl::getAllCommittedTimestamp(ServiceContext* serviceCtx) const {
    return serviceCtx->getStorageEngine()->getAllCommittedTimestamp();
}

}  // namespace repl
}  // namespace mongo
